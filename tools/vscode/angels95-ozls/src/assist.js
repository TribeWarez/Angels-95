'use strict';

const vscode = require('vscode');
const { getModel } = require('./cache');
const D = require('./data');

const WORD_RE = /[$@A-Za-z_][$\w.]*/;

function hoverResult(blocks, range) {
  const text = blocks.filter(Boolean).join('\n\n');
  if (!text) return null;
  return new vscode.Hover(new vscode.MarkdownString(text), range);
}

function wordAt(document, position) {
  const range = document.getWordRangeAtPosition(position, WORD_RE);
  if (!range) return null;
  return { word: document.getText(range), range };
}

function plural(n, one, many) {
  return `${n} ${n === 1 ? one : (many || one + 's')}`;
}

function variableDoc(word) {
  if (word === '$result') {
    return '**`$result`** — output slot for `rtflag` and `get_worldflag`.';
  }
  if (/^\$flag\d+$/.test(word)) {
    return '**Toggle flag** — 0..63. Read with `rtflag`, write with `wtflag`, flip with `toggle_flag`.';
  }
  return '**Script variable.** Resolution order: float `var` → int `var` → toggle flag → external stat (`$health`, `$mana`, `$ammo`, …) → 0.';
}

function pawnHover(document, position, hit) {
  const line = document.lineAt(position.line).text;
  const kv = /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$/.exec(line);
  if (kv && kv[1] === hit.word) {
    const doc = D.CFG_KEY_DOCS[kv[1]];
    return hoverResult([
      `**${kv[1]}=** — ${doc || 'Unknown pawn-def key. The engine ignores it.'}`,
      doc ? `Current value: \`${kv[2].trim()}\`` : null,
      D.NUMERIC_CFG_KEYS.has(kv[1]) && kv[2].trim() !== '' && !/^[+-]?(?:\d+\.?\d*|\.\d+)$/.test(kv[2].trim())
        ? '**This value is not a number.** The client and editor use `std::stof` without a try/catch, so this terminates the process.'
        : null
    ], hit.range);
  }
  if (/^\s*\[/.test(line)) {
    return hoverResult([
      '**Roster section** — one section per character. The section name is the stable id stored in `System/PlayerProfiles.ini` and sent on the wire.',
      'Keys: `name`, `mesh`, `texture`, `mesh_type`, `portrait`, `bio`, `accent`, `anim_idle`, `anim_chase`.'
    ], hit.range);
  }
  if (D.CFG_KEY_DOCS[hit.word]) {
    return hoverResult([`**${hit.word}** — ${D.CFG_KEY_DOCS[hit.word]}`], hit.range);
  }
  return null;
}

function ozlsHover(document, position, hit) {
  const model = getModel(document);
  const word = hit.word;
  const line = document.lineAt(position.line).text;
  const blocks = [];

  // Entity header line.
  if (model.entity && model.entity.line === position.line) {
    if (word === 'entity') {
      blocks.push('**entity** — the file header. One def per file; the name is a quoted string BEFORE the colon.');
    } else if (word === model.entity.name) {
      blocks.push(`**${model.entity.name}** — def name. OZONE \`npc\`/\`pickup\`/\`zone name=\`/weapon names resolve against this.`);
    } else if (word === model.entity.type) {
      blocks.push(`**${model.entity.type}** — ${D.ENTITY_TYPE_DOCS[model.entity.type] || 'Unknown type; the def is dropped.'}`);
    }
  }

  // stats / actions / variants block keywords.
  if (model.stats && model.stats.line === position.line && word === 'stats') {
    blocks.push('**stats { }** — flat `key = value` pairs. There is no `vec3` prefix: write `colour = (r, g, b)`.');
  }
  if (model.actions && model.actions.line === position.line && word === 'actions') {
    blocks.push('**actions { }** — one `name { … }` block per hook. Action names become script labels and must match `[A-Za-z0-9_]+`.');
  }
  if (model.variants && model.variants.line === position.line && word === 'variants') {
    blocks.push('**variants { }** — weapon tiers selected by player level. Only `mesh_override` and `texture_override` are read.');
  }

  // A stat key.
  const stat = model.stats && model.stats.entries.find((e) => e.line === position.line);
  if (stat && word === stat.key) {
    blocks.push(`**${stat.key}** — ${D.STAT_DOCS[stat.key] || 'The parser accepts any stat key; this one has no editor schema entry.'}`);
    blocks.push(`Value: \`${stat.value || '(empty → float 0)'}\`  (${stat.valueKind})`);
  }

  // A top-level property key.
  const prop = model.properties.find((p) => p.line === position.line);
  if (prop && word === prop.key) {
    blocks.push(`**${prop.key}** — ${D.TOP_KEY_DOCS[prop.key] || 'Known top-level key.'}`);
    blocks.push(`Value: \`${prop.value}\``);
  }

  // A hook declaration.
  const hook = model.actions && model.actions.hooks.find((h) => h.line === position.line);
  if (hook && word === hook.name) {
    blocks.push(`**${hook.name}** — ${D.HOOK_DOCS[hook.name] || 'Action block. It runs only when something jumps to its label or emits a matching event.'}`);
    blocks.push(`${plural(hook.body.length, 'line')} in this action.`);
  }

  // An action body line.
  const bodyHook = model.actions && model.actions.hooks.find((h) => h.body.some((b) => b.line === position.line));
  if (bodyHook) {
    if (word.startsWith('$')) {
      blocks.push(variableDoc(word));
    } else if (D.OPCODE_DOCS[word]) {
      blocks.push(`**${word}** — ${D.OPCODE_DOCS[word]}`);
    } else if (D.HOOK_DOCS[word]) {
      blocks.push(`**${word}** — ${D.HOOK_DOCS[word]}  Referenced from this action.`);
    } else if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(word) && line.trim().endsWith(word + ':')) {
      blocks.push(`**${word}** — script label. \`goto\`/\`jump ${word}\` targets it.`);
    }
  }

  // Variant names and override keys.
  if (model.variants) {
    const variant = model.variants.variants.find((v) => v.line === position.line);
    if (variant && word === variant.name) {
      blocks.push(`**Variant ${variant.name}** — selected by its \`lvlN\` prefix, highest tier <= player level.`);
    }
    for (const v of model.variants.variants) {
      for (const k of v.keys) {
        if (k.line === position.line && word === k.key) {
          blocks.push(k.key === 'mesh_override'
            ? '**mesh_override** — model path used for this tier.'
            : k.key === 'texture_override'
              ? '**texture_override** — texture path used for this tier.'
              : `**${k.key}** — variants ignore anything except \`mesh_override\` / \`texture_override\`.`);
        }
      }
    }
  }

  // Generic fallbacks (hovering a keyword anywhere).
  if (!blocks.length) {
    if (D.HOOK_DOCS[word]) blocks.push(`**${word}** — ${D.HOOK_DOCS[word]}`);
    else if (D.OPCODE_DOCS[word]) blocks.push(`**${word}** — ${D.OPCODE_DOCS[word]}`);
    else if (D.TOP_KEY_DOCS[word]) blocks.push(`**${word}** — ${D.TOP_KEY_DOCS[word]}`);
    else if (D.ENTITY_TYPE_DOCS[word]) blocks.push(`**${word}** — ${D.ENTITY_TYPE_DOCS[word]}`);
    else if (word.startsWith('$')) blocks.push(variableDoc(word));
  }

  return blocks.length ? hoverResult(blocks, hit.range) : null;
}

function provideHover(document, position) {
  const hit = wordAt(document, position);
  if (!hit) return null;
  return document.languageId === 'ozpawn'
    ? pawnHover(document, position, hit)
    : ozlsHover(document, position, hit);
}

function provideCodeLenses(document) {
  const model = getModel(document);
  if (!model) return [];

  if (model.kind === 'ozpawn') {
    const entries = model.root.entries.concat(...model.sections.map((s) => s.entries));
    const name = entries.find((e) => e.key === 'name');
    if (!name) return [];
    return [new vscode.CodeLens(new vscode.Range(name.line, 0, name.line, 0), {
      title: `pawn: ${name.value}`,
      tooltip: 'Pawn def name',
      command: 'angels95Ozd.reveal',
      arguments: [document.uri, name.line]
    })];
  }

  if (!model.entity) return [];
  const uri = document.uri;
  const out = [];
  const add = (line, title, targetLine, tooltip) => {
    out.push(new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
      title,
      tooltip,
      command: 'angels95Ozd.reveal',
      arguments: [uri, targetLine === undefined ? line : targetLine]
    }));
  };

  if (model.stats) {
    add(model.stats.line, plural(model.stats.entries.length, 'stat'), model.stats.line,
      'Reveal the stats block');
  }
  if (model.actions) {
    add(model.actions.line, plural(model.actions.hooks.length, 'action'), model.actions.line,
      'Reveal the actions block');
    for (const h of model.actions.hooks) {
      add(h.line, plural(h.body.length, 'line'), h.line, `Reveal ${h.name}`);
    }
  }
  if (model.variants) {
    add(model.variants.line, plural(model.variants.variants.length, 'variant'), model.variants.line,
      'Reveal the variants block');
  }
  return out;
}

module.exports = { provideHover, provideCodeLenses };
