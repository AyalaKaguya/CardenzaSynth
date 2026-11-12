#include "Devices/Device.h"
#include "Devices/MosaicoNative/StorageBackend.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_nand_flash.h"
#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

namespace {
constexpr char kTag[] = "NativeStorage";
constexpr char kMount[] = "/nand";
constexpr char kAppParent[] = "/nand/apps";
constexpr char kRoot[] = "/nand/apps/mosaico-native";
constexpr std::int64_t kUsageCacheMicros = 2000000;

// Vibe Mode and the app run separately; their in-memory VFS owners never coexist.
// The existing filesystem contents and its persisted geometry are shared.
struct StorageState {
  std::mutex mutex;
  esp_blockdev_handle_t blockdev = nullptr;
  spi_device_handle_t spi = nullptr;
  bool busOwned = false;
  bool attempted = false;
  bool volumeMounted = false;
  std::int64_t usageAt = 0;
  const esp_blockdev_ops_t* originalOps = nullptr;
  esp_blockdev_ops_t vfsOps{};
};
StorageState storage;
std::atomic<bool> available{false};
std::atomic<esp_err_t> lastError{ESP_ERR_INVALID_STATE};
std::atomic<std::uint64_t> volumeBytes{0};
std::atomic<std::uint64_t> capacityBytes{0};
std::atomic<std::uint64_t> freeBytes{0};

int ProbeRead(const lfs_config* config, lfs_block_t block, lfs_off_t offset, void* buffer, lfs_size_t size)
{
  auto source = static_cast<esp_blockdev_handle_t>(config->context);
  const std::uint64_t address = static_cast<std::uint64_t>(block) * config->block_size + offset;
  if (offset > config->block_size || size > config->block_size - offset ||
      address > source->geometry.disk_size || size > source->geometry.disk_size - address) return LFS_ERR_IO;
  return source->ops->read(source, static_cast<std::uint8_t*>(buffer), size, address, size) == ESP_OK ? 0 : LFS_ERR_IO;
}
int ProbeWrite(const lfs_config*, lfs_block_t, lfs_off_t, const void*, lfs_size_t) { return LFS_ERR_IO; }
int ProbeErase(const lfs_config*, lfs_block_t) { return LFS_ERR_IO; }
int ProbeSync(const lfs_config*) { return 0; }

esp_err_t StoredGeometry(esp_blockdev_handle_t source, std::uint64_t& bytes)
{
  if (source == nullptr || source->ops == nullptr || source->ops->read == nullptr) return ESP_ERR_INVALID_ARG;
  const std::size_t page = source->geometry.read_size;
  if (page < 128 || page > UINT32_MAX || (page & (page - 1)) != 0 ||
      source->geometry.write_size != page || source->geometry.erase_size != page ||
      source->geometry.disk_size == 0 || source->geometry.disk_size % page != 0) return ESP_ERR_INVALID_SIZE;

  // Probe the persisted superblock through callbacks that refuse every write.
  // This view is unmounted before transferring the handle to the VFS.
  auto* config = static_cast<lfs_config*>(std::calloc(1, sizeof(lfs_config)));
  auto* fs = static_cast<lfs_t*>(std::calloc(1, sizeof(lfs_t)));
  auto* info = static_cast<lfs_fsinfo*>(std::calloc(1, sizeof(lfs_fsinfo)));
  if (config == nullptr || fs == nullptr || info == nullptr)
  {
    std::free(info); std::free(fs); std::free(config);
    return ESP_ERR_NO_MEM;
  }
  config->context = source;
  config->read = ProbeRead;
  config->prog = ProbeWrite;
  config->erase = ProbeErase;
  config->sync = ProbeSync;
  config->read_size = page;
  config->prog_size = page;
  config->block_size = page;
  config->block_count = 0;
  config->cache_size = page;
  config->lookahead_size = 16;
  config->block_cycles = -1;
  int result = lfs_mount(fs, config);
  if (result == 0)
  {
    result = lfs_fs_stat(fs, info);
    const int unmounted = lfs_unmount(fs);
    if (result == 0) result = unmounted;
  }
  esp_err_t error = result == LFS_ERR_NOMEM ? ESP_ERR_NO_MEM : result != 0 ? ESP_FAIL : ESP_OK;
  if (error == ESP_OK)
  {
    const std::uint64_t stored = static_cast<std::uint64_t>(info->block_count) * info->block_size;
    if (info->block_count == 0 || info->block_size != page || stored > source->geometry.disk_size)
      error = ESP_ERR_INVALID_SIZE;
    else bytes = stored;
  }
  std::free(info); std::free(fs); std::free(config);
  return error;
}

esp_err_t ReleaseUnmounted()
{
  if (storage.blockdev != nullptr && !storage.volumeMounted)
  {
    const esp_err_t result = storage.blockdev->ops->release(storage.blockdev);
    if (result != ESP_OK) return result;
    storage.blockdev = nullptr;
  }
  if (storage.spi != nullptr)
  {
    const esp_err_t result = spi_bus_remove_device(storage.spi);
    if (result != ESP_OK) return result;
    storage.spi = nullptr;
  }
  if (storage.busOwned)
  {
    const esp_err_t result = spi_bus_free(BSP_NAND_SPI_HOST);
    if (result != ESP_OK) return result;
    storage.busOwned = false;
  }
  return ESP_OK;
}

esp_err_t ReleaseFromVfs(esp_blockdev_handle_t device)
{
  // Register can fail before accepting the handle, or after freeing its VFS
  // context. Observe the actual release instead of assuming every failure
  // consumes it; all other ops still receive the original driver handle.
  const auto release = storage.originalOps->release;
  device->ops = storage.originalOps;
  const esp_err_t result = release(device);
  if (result == ESP_OK) storage.blockdev = nullptr;
  else device->ops = &storage.vfsOps;
  return result;
}

esp_err_t ReleasePin(gpio_num_t pin)
{
  gpio_config_t config{};
  config.pin_bit_mask = UINT64_C(1) << pin;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  esp_err_t result = gpio_set_level(pin, 1);
  if (result == ESP_OK) result = gpio_config(&config);
  return result;
}

bool EnsureDirectory(const char* path)
{
  struct stat info{};
  if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
  return errno == ENOENT && mkdir(path, 0775) == 0;
}

esp_err_t MountVolume()
{
  // The fixed BSP raw-device API cannot be used with the block-device layer.
  // Use its public pins with the official layered driver; refuse a foreign owner.
  if (bsp_nand_flash_get_handle() != nullptr) return ESP_ERR_INVALID_STATE;
  esp_err_t result = ReleasePin(BSP_NAND_HOLD);
  if (result == ESP_OK) result = ReleasePin(BSP_NAND_WP);
  if (result != ESP_OK) return result;
  spi_bus_config_t bus{};
  bus.mosi_io_num = BSP_NAND_D;
  bus.miso_io_num = BSP_NAND_Q;
  bus.sclk_io_num = BSP_NAND_CLK;
  bus.quadhd_io_num = GPIO_NUM_NC;
  bus.quadwp_io_num = GPIO_NUM_NC;
  bus.max_transfer_sz = 4096;
  result = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) return result;
  storage.busOwned = true;
  spi_device_interface_config_t device{};
  device.clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ;
  device.mode = 0;
  device.spics_io_num = BSP_NAND_CS;
  device.queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE;
  device.flags = SPI_DEVICE_HALFDUPLEX;
  result = spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &storage.spi);
  if (result == ESP_OK)
  {
    spi_nand_flash_config_t config{};
    config.device_handle = storage.spi;
    config.gc_factor = 0;
    config.io_mode = SPI_NAND_IO_MODE_SIO;
    config.flags = SPI_DEVICE_HALFDUPLEX;
    result = spi_nand_flash_init_with_layers(&config, &storage.blockdev);
  }
  std::uint64_t stored = 0;
  if (result == ESP_OK) result = StoredGeometry(storage.blockdev, stored);
  if (result == ESP_OK)
  {
    // A recovered Dhara map can report more capacity than the existing volume.
    // Preserve the superblock's original block count instead of growing it.
    storage.blockdev->geometry.disk_size = stored;
    storage.originalOps = storage.blockdev->ops;
    storage.vfsOps = *storage.originalOps;
    storage.vfsOps.release = ReleaseFromVfs;
    storage.blockdev->ops = &storage.vfsOps;
    esp_vfs_littlefs_conf_t config{};
    config.base_path = kMount;
    config.blockdev = storage.blockdev;
    config.format_if_mount_failed = false;
    config.read_only = false;
    config.dont_mount = false;
    config.grow_on_mount = false;
    result = esp_vfs_littlefs_register(&config);
    if (result == ESP_OK)
    {
      storage.volumeMounted = true;
      volumeBytes.store(stored, std::memory_order_relaxed);
    }
  }
  if (result != ESP_OK)
  {
    const esp_err_t cleanup = ReleaseUnmounted();
    if (cleanup != ESP_OK) ESP_LOGE(kTag, "partial NAND resources retained: %s", esp_err_to_name(cleanup));
    return result;
  }
  // OS/Storage treats its root as already existing and only creates descendants.
  if (!EnsureDirectory(kAppParent) || !EnsureDirectory(kRoot)) return ESP_FAIL;
  ESP_LOGI(kTag, "existing NAND mounted, geometry=%llu bytes; app root=%s",
           static_cast<unsigned long long>(stored), kRoot);
  return ESP_OK;
}

void RefreshUsage()
{
  if (!available.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(storage.mutex);
  const std::int64_t now = esp_timer_get_time();
  if (storage.usageAt != 0 && now - storage.usageAt < kUsageCacheMicros) return;
  std::size_t total = 0, used = 0;
  if (esp_littlefs_blockdev_info(storage.blockdev, &total, &used) == ESP_OK)
  {
    capacityBytes.store(total, std::memory_order_relaxed);
    freeBytes.store(used <= total ? total - used : 0, std::memory_order_relaxed);
    storage.usageAt = now;
  }
}
}  // namespace

namespace mosaico::storage {
esp_err_t Error() { return lastError.load(std::memory_order_acquire); }
std::uint64_t MountedVolumeBytes() { return volumeBytes.load(std::memory_order_relaxed); }
}  // namespace mosaico::storage

namespace Device::Storage {
std::string RootPath() { return kRoot; }
bool Mount()
{
  std::lock_guard<std::mutex> lock(storage.mutex);
  if (storage.attempted) return available.load(std::memory_order_acquire);
  storage.attempted = true;
  const esp_err_t result = MountVolume();
  lastError.store(result, std::memory_order_release);
  available.store(result == ESP_OK, std::memory_order_release);
  if (result != ESP_OK) ESP_LOGE(kTag, "NAND unavailable without format/repair: %s", esp_err_to_name(result));
  return result == ESP_OK;
}
bool IsMounted() { return available.load(std::memory_order_acquire); }
void CheckAfterIoError()
{
  if (!available.load(std::memory_order_acquire)) return;
  struct stat info{};
  if (stat(kRoot, &info) != 0 || !S_ISDIR(info.st_mode))
  {
    // Do not tear down a mounted device while the sample worker owns open files.
    available.store(false, std::memory_order_release);
    lastError.store(ESP_FAIL, std::memory_order_release);
    ESP_LOGE(kTag, "application storage stopped answering; restart required");
  }
}
std::uint64_t CapacityBytes() { RefreshUsage(); return capacityBytes.load(std::memory_order_relaxed); }
std::uint64_t FreeBytes() { RefreshUsage(); return freeBytes.load(std::memory_order_relaxed); }
}  // namespace Device::Storage
