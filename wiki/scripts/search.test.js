// The manual's search regression: everyday questions, in both languages, must find the section that
// answers them among the first three results, and every title its own section. After a build:
//   npm run test:search        (node scripts/search.test.js build)
const path = require('path');

const dir = path.resolve(process.argv[2] || 'build');
const LearnSearch = require('../src/search/learn-search.js');
const index = LearnSearch.createIndex(require(path.join(dir, 'search-index.json')));

const cases = [
  ['怎么调音量', 'zh', 'fn-quick'], ['声音太小', 'zh', 'fn-quick'], ['volume', 'en', 'fn-quick'], ['louder', 'en', 'fn-quick'],
  ['bpm', 'zh', 'fn-quick'], ['tap tempo', 'en', 'fn-shortcuts'], ['save', 'en', 'project-save'], ['怎么保存', 'zh', 'project-save'],
  ['how to save my project', 'en', 'project-save'], ['record', 'en', 'seq-transport'], ['录音', 'zh', 'seq-transport'],
  ['怎么录音', 'zh', 'seq-transport'], ['录音设置', 'zh', 'record-panel'], ['刚才弹的找回来', 'zh', 'capture'], ['capture', 'en', 'capture'],
  ['步进录制', 'zh', 'seq-step-rec'], ['步进音序', 'zh', 'seq-step-rec'], ['piano roll', 'en', 'seq-roll'], ['钢琴卷帘', 'zh', 'seq-roll'],
  ['mute', 'en', 'tape-mix'], ['静音', 'zh', 'tape-mix'], ['和弦', 'zh', 'play-chords'], ['arpegiator', 'en', 'play-arp'],
  ['没声音', 'zh', 'faq-no-sound'], ['no sound', 'en', 'faq-no-sound'], ['卡音', 'zh', 'faq-stuck'], ['撤销', 'zh', 'faq-undo'],
  ['undo', 'en', 'faq-undo'], ['midi', 'en', 'faq-midi'], ['八度', 'zh', 'play-octave'], ['transpose', 'en', 'play-octave'],
  ['音阶锁定', 'zh', 'play-lock'], ['scale', 'en', 'play-scale'], ['电脑键盘', 'zh', 'web-keys'], ['fn面板', 'zh', 'faq-settings'],
  ['sd卡', 'zh', 'storage-card'], ['delete pattern', 'en', 'seq-patterns'], ['换音色', 'zh', 'instr-voice'], ['reverb', 'en', 'fx-sends'],
  ['sequencer', 'en', 'seq'], ['灯为什么全亮', 'zh', 'lights-states'], ['led brightness', 'en', 'lights-ramp'],
  ['屏幕亮度', 'zh', 'setup-system'], ['usb 连接', 'zh', 'web-usb'], ['connect my cardenza', 'en', 'web-usb'], ['跟着操作', 'zh', 'web-usb'], ['try it live', 'en', 'web'], ['自动保存', 'zh', 'project-session'], ['no sd card', 'en', 'storage-card'], ['日志', 'zh', 'storage-log'], ['next pattern', 'en', 'seq-long'], ['dx7 算法', 'zh', 'engine-dx7'], ['juno filter', 'en', 'engine-juno'], ['采样', 'zh', 'sampler'],
];

let failed = 0;
for (const [query, lang, want] of cases) {
  const ids = LearnSearch.search(index, query, { lang, limit: 3 }).map((r) => r.id);
  if (!ids.includes(want)) {
    failed++;
    console.error(`search: "${query}" [${lang}] should find #${want}, got ${ids.join(', ') || 'nothing'}`);
  }
}
// Every section must be reachable by its own title.
for (const section of index.sections) {
  const ids = LearnSearch.search(index, section.title, { lang: section.lang, limit: 8 }).map((r) => r.id);
  if (!ids.includes(section.id)) {
    failed++;
    console.error(`search: the title "${section.title}" [${section.lang}] does not find #${section.id}`);
  }
}
if (failed) {
  console.error(`search: ${failed} failure(s)`);
  process.exit(1);
}
console.log(`search: ${cases.length} questions and ${index.sections.length} titles found their sections`);
