// The scene map: every page of every scene with its screenshot (./scene-map.ts), each linking to its
// section. The device map is ./board.tsx.
import React from 'react';
import Link from '@docusaurus/Link';
import {SCENE_MAP} from './scene-map';
import {useLang, useUrls} from './common';

const HOLD = {zh: '按住左侧键', en: 'Hold a left key'};

export function SceneMap() {
  const lang = useLang();
  const urls = useUrls();
  return (
    <div className="cz-scene-map-box"><div className="cz-scene-map">
      {SCENE_MAP.map(([title, section, items]) => (
        <section key={title} className={title === 'HOLD' ? 'scene-col scene-row' : 'scene-col'}>
          <Link className="scene-title" to={urls.sectionPath(section)}>{title === 'HOLD' ? HOLD[lang] : title}</Link>
          {items.map(([label, sec, shot]) => (
            <Link key={label} className="scene-card" to={urls.sectionPath(sec)}>
              <img loading="lazy" src={urls.shot(shot)} width="480" height="270" alt={label} />
              <span>{label}</span>
            </Link>
          ))}
        </section>
      ))}
    </div></div>
  );
}
