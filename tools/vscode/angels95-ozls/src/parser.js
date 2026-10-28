'use strict';

const {
  ENTITY_TYPES, TOP_KEYS, BLOCK_KEYS, KNOWN_HOOKS, OPCODES,
  NUMERIC_CFG_KEYS, BOOL_CFG_KEYS, INT_CFG_KEYS, CFG_KEYS
} = require('./data');

const NUMBER_RE = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;
const VEC3_RE = /^\(\s*[+-]?(?:\d+\.?\d*|\.\d+)\s*[,;]\s*[+-]?(?:\d+\.?\d*|\.\d+)\s*[,;]\s*[+-]?(?:\d+\.?\d*|\.\d+)\s*\)$/;

/** Remove // and # comments, but only at a token start and never inside a quoted string. */
function stripComments(line) {
  let inString = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c === '#' || (c === '/' && line[i + 1] === '/')) {
      if (i === 0 || /\s/.test(line[i - 1])) return line.slice(0, i);
    }
  }
  return line;
}

function braceDelta(line) {
  let depth = 0;
  let inString = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c === '{') depth++;
    else if (c === '}') depth--;
  }
  return depth;
}

function problem(list, line, severity, message) {
  list.push({ line, severity, message });
}

function classifyValue(value) {
  if (value === '') return 'float';
  if (value.startsWith('(')) return 'vec3';
  if (NUMBER_RE.test(value)) return 'float';
  return 'string';
}

/* --------------------------------------------------------------------- .ozls */

function parseOzls(text) {
  const source = text.replace(/^\uFEFF/, '');
  const lines = source.split(/\r?\n/);
  const model = {
    kind: 'ozls',
    entity: null,
    properties: [],
    stats: null,
    actions: null,
    variants: null,
    problems: [],
    closed: true
  };

  if (text.charCodeAt(0) === 0xfeff) {
    problem(model.problems, 0, 'warning',
      'UTF-8 BOM detected. LightningScriptParser does not strip a BOM, so the first token becomes "\\uFEFFentity" and the whole file is rejected. Save as UTF-8 without BOM.');
  }

  let depth = 0;
  let state = 'outside';
  let currentHook = null;
  let currentVariant = null;
  let secondEntity = false;
  let headerReported = false;

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i].replace(/\r$/, '');
    const code = stripComments(raw).trim();
    const startDepth = depth;
    const endDepth = startDepth + braceDelta(stripComments(raw));
    const isClose = /^\}+$/.test(code);

    // --- entity header -------------------------------------------------
    if (startDepth === 0 && /^\s*entity\b/.test(code)) {
      const hm = /^\s*entity\s+"([^"]*)"\s*:\s*([A-Za-z0-9_.]+)\s*\{?/.exec(code);
      if (!model.entity) {
        if (hm) {
          model.entity = {
            name: hm[1],
            type: hm[2],
            line: i,
            lineCount: 1,
            validType: ENTITY_TYPES.has(hm[2])
          };
          if (!model.entity.validType) {
            problem(model.problems, i, 'error',
              `Unknown entity type '${hm[2]}'. LightningScriptParser marks the def UNKNOWN and ParseAll drops it.`);
          }
          if (!hm[1]) {
            problem(model.problems, i, 'error', 'The entity name must be a non-empty quoted string.');
          }
        } else {
          problem(model.problems, i, 'error',
            'Malformed entity header. Expected: entity "Name" : type { — the name must be double-quoted.');
          model.entity = { name: '?', type: 'UNKNOWN', line: i, lineCount: 1, validType: false };
        }
        state = 'entity';
      } else if (!secondEntity) {
        secondEntity = true;
        problem(model.problems, i, 'info',
          'A second entity block is ignored: one .ozls file holds exactly one def.');
        state = 'ignored';
      }
      depth = endDepth;
      continue;
    }

    if (!model.entity) {
      if (code && !headerReported) {
        headerReported = true;
        problem(model.problems, i, 'error',
          'File does not start with `entity "Name" : type {`. LightningScriptParser returns an empty def.');
      }
      depth = endDepth;
      continue;
    }

    if (state === 'ignored') {
      depth = endDepth;
      continue;
    }

    // --- block closes -------------------------------------------------
    if (isClose) {
      if (state === 'actions') {
        if (currentHook && endDepth <= currentHook.depth) currentHook = null;
        if (endDepth <= 1) {
          state = 'entity';
          currentHook = null;
        }
      } else if (state === 'variants') {
        if (currentVariant && endDepth <= currentVariant.depth) currentVariant = null;
        if (endDepth <= 1) {
          state = 'entity';
          currentVariant = null;
        }
      } else if (state === 'stats' && endDepth <= 1) {
        state = 'entity';
      }
      depth = endDepth;
      continue;
    }

    // --- entity body (depth 1) ---------------------------------------
    if (state === 'entity' && startDepth === 1) {
      const block = /^(stats|actions|variants)\s*\{/.exec(code);
      if (block) {
        const name = block[1];
        if (name === 'stats') {
          if (model.stats) {
            problem(model.problems, i, 'info', 'A second `stats` block REPLACES the first one.');
          }
          model.stats = { line: i, entries: [] };
        } else if (name === 'actions') {
          model.actions = model.actions || { line: i, hooks: [] };
        } else {
          model.variants = model.variants || { line: i, variants: [] };
        }
        state = name === 'stats' ? 'stats' : name === 'actions' ? 'actions' : 'variants';
        depth = endDepth;
        continue;
      }

      const kv = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/.exec(code);
      if (kv) {
        const key = kv[1];
        if (!TOP_KEYS.has(key)) {
          problem(model.problems, i, 'warning',
            `Unknown top-level key '${key}'. The .ozls body parser warns and DROPS it — put custom fields inside stats { }.`);
        }
        model.properties.push({ key, value: kv[2], line: i });
        depth = endDepth;
        continue;
      }

      if (code && !/^\{/.test(code)) {
        const token = code.split(/\s+/)[0];
        problem(model.problems, i, 'warning',
          `Unexpected token '${token}' at entity-body level. Only top-level keys and stats/actions/variants blocks are valid here.`);
      }
      depth = endDepth;
      continue;
    }

    // --- stats --------------------------------------------------------
    if (state === 'stats' && startDepth === 2) {
      if (code.startsWith('vec3')) {
        problem(model.problems, i, 'error',
          'A `vec3` prefix is not valid. Write `name = (r, g, b)`; the parser sees key `vec3`, expects `=`, and ABORTS the rest of the stats block.');
        depth = endDepth;
        continue;
      }
      const kv = /^([^\s{}=:]+)\s*=\s*(.*)$/.exec(code);
      if (!kv) {
        if (code) {
          problem(model.problems, i, 'warning',
            'Expected `key = value` inside stats { }. A missing `=` stops the block and loses every later stat.');
        }
        depth = endDepth;
        continue;
      }
      const key = kv[1];
      const value = kv[2];
      const valueKind = classifyValue(value);
      if (valueKind === 'vec3' && !VEC3_RE.test(value)) {
        problem(model.problems, i, 'error',
          `Malformed vec3 for '${key}'. The parser skips THIS value (leaves it unset) and keeps parsing. Use (r, g, b) or (r,g,b).`);
      }
      if (valueKind === 'string' && /\s/.test(value)) {
        problem(model.problems, i, 'warning',
          `Stat '${key}' has a value containing a space. The parser stops at the first whitespace, so only '${value.split(/\s+/)[0]}' is stored.`);
      }
      if (valueKind === 'string' && /^".*\s.*"$/.test(value)) {
        problem(model.problems, i, 'warning',
          `Quoted stat value for '${key}' contains a space. Quotes are stored VERBATIM in stats, and the value is truncated at the first space.`);
      }
      model.stats.entries.push({ key, value, line: i, valueKind });
      depth = endDepth;
      continue;
    }

    // --- actions ------------------------------------------------------
    if (state === 'actions') {
      if (currentHook && startDepth >= currentHook.depth + 1) {
        if (code && code !== '{' && !/^\}+\s*$/.test(code)) {
          const first = code.split(/\s+/)[0];
          const isLabel = /^[A-Za-z_][A-Za-z0-9_]*:(?!:)/.test(code);
          const isVarAssign = /^\$[A-Za-z_][A-Za-z0-9_]*\s*(?:[-+*/])?=/.test(code);
          if (!isLabel && !isVarAssign && /^[A-Za-z_][A-Za-z0-9_]*$/.test(first) && !OPCODES.has(first)) {
            problem(model.problems, i, 'info',
              `Unknown opcode '${first}'. LightningScriptContext warns 'unknown opcode' at runtime and skips the line.`);
          }
          currentHook.body.push({ text: code, line: i, opcode: isLabel ? '(label)' : isVarAssign ? '$assign' : first });
        }
        depth = endDepth;
        continue;
      }

      if (startDepth === 2) {
        const hook = /^("([^"]*)"|([A-Za-z0-9_.-]+))\s*\{/.exec(code);
        if (hook) {
          const name = hook[2] !== undefined ? hook[2] : hook[3];
          if (!/^[A-Za-z0-9_]+$/.test(name)) {
            problem(model.problems, i, 'warning',
              `Action name '${name}' is not a valid script label. A name containing '.' or '-' can NEVER fire: labels must match [A-Za-z0-9_]+.`);
          } else if (!KNOWN_HOOKS.has(name) && !/^on_/.test(name)) {
            problem(model.problems, i, 'info',
              `Action '${name}' is a custom label. It only runs if something does goto/jump to it or emits the matching event.`);
          }
          currentHook = { name, line: i, depth: startDepth, body: [] };
          model.actions.hooks.push(currentHook);
        } else if (code) {
          problem(model.problems, i, 'warning',
            'Expected an action block: `name {` inside actions { }.');
        }
        depth = endDepth;
        continue;
      }
    }

    // --- variants -----------------------------------------------------
    if (state === 'variants') {
      if (currentVariant && startDepth >= currentVariant.depth + 1) {
        const kv = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/.exec(code);
        if (kv) {
          if (kv[1] !== 'mesh_override' && kv[1] !== 'texture_override') {
            problem(model.problems, i, 'warning',
              `Unknown variant key '${kv[1]}'. Variants silently ignore anything except mesh_override / texture_override.`);
          }
          currentVariant.keys.push({ key: kv[1], value: kv[2], line: i });
        }
        depth = endDepth;
        continue;
      }
      if (startDepth === 2) {
        const vm = /^("([^"]*)"|([A-Za-z0-9_]+))\s*\{/.exec(code);
        if (vm) {
          currentVariant = { name: vm[2] !== undefined ? vm[2] : vm[3], line: i, depth: startDepth, keys: [] };
          model.variants.variants.push(currentVariant);
        }
        depth = endDepth;
        continue;
      }
    }

    depth = endDepth;
  }

  if (model.entity) {
    model.entity.lineCount = lines.length - model.entity.line;
    if (depth !== 0) {
      model.closed = false;
      problem(model.problems, model.entity.line, 'warning', 'Unclosed block: the file ends before every `}` is closed.');
    }
  }

  return model;
}

/* ---------------------------------------------------------------------- .cfg */

function parsePawnDef(text) {
  const lines = text.split(/\r?\n/);
  const model = {
    kind: 'ozpawn',
    sections: [],
    root: { name: '', line: 0, entries: [] },
    problems: []
  };
  let current = model.root;

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i].replace(/\r$/, '');
    const trimmed = raw.trim();
    if (!trimmed) continue;
    if (trimmed.startsWith(';') || trimmed.startsWith('#')) continue;

    const section = /^\[([^\]]*)\]/.exec(trimmed);
    if (section) {
      current = { name: section[1], line: i, entries: [] };
      model.sections.push(current);
      continue;
    }

    const eq = raw.indexOf('=');
    if (eq < 0) {
      problem(model.problems, i, 'warning', 'Expected `key=value`.');
      continue;
    }
    const key = raw.slice(0, eq).trim();
    const value = raw.slice(eq + 1).trim();
    if (!key) continue;

    if (!CFG_KEYS.has(key)) {
      problem(model.problems, i, 'info', `Unknown pawn-def key '${key}'. The engine ignores it.`);
    }
    if (NUMERIC_CFG_KEYS.has(key) && value !== '' && !NUMBER_RE.test(value)) {
      problem(model.problems, i, 'error',
        `'${key} = ${value}' is not a number. The CLIENT and EDITOR use std::stof without a try/catch, so a bad value terminates the process; the server logs and keeps the default.`);
    }
    if (INT_CFG_KEYS.has(key) && value !== '' && !/^[+-]?\d+$/.test(value)) {
      problem(model.problems, i, 'error', `'${key}' must be an integer.`);
    }
    current.entries.push({ key, value, line: i });
  }

  const flat = model.root.entries.concat(...model.sections.map((s) => s.entries));
  if (!flat.some((e) => e.key === 'name' && e.value)) {
    problem(model.problems, 0, 'warning', 'No `name=` key. A pawn def without a name is skipped silently.');
  }

  return model;
}

function parseAny(text, languageId) {
  return languageId === 'ozpawn' ? parsePawnDef(text) : parseOzls(text);
}

module.exports = {
  stripComments,
  braceDelta,
  parseOzls,
  parsePawnDef,
  parseAny,
  NUMBER_RE,
  VEC3_RE
};
