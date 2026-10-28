'use strict';

const { PRIMITIVES, ENTITIES } = require('./data');

const CSG_OPS = new Set(['add', 'sub', 'intersect', 'deresc']);
const BRUSH_KINDS = new Set(Object.keys(PRIMITIVES));
const ENTITY_KINDS = new Set(Object.keys(ENTITIES));
const SUBTYPED = new Set(['zone', 'light']);

function tokenize(line) {
  const out = [];
  let i = 0;
  while (i < line.length) {
    while (i < line.length && (line[i] === ' ' || line[i] === '\t')) i++;
    if (i >= line.length) break;
    const start = i;
    let text = '';
    while (i < line.length && line[i] !== ' ' && line[i] !== '\t') {
      if (line[i] === '"') {
        text += line[i++];
        while (i < line.length) {
          text += line[i];
          if (line[i] === '"') { i++; break; }
          i++;
        }
      } else {
        text += line[i++];
      }
    }
    out.push({ text, start, end: i });
  }
  return out;
}

function isNumber(text) {
  return /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/.test(text);
}

/**
 * Parse one OZONE source line. Deliberately shallow: it mirrors the token
 * grammar the engine accepts, without any world semantics.
 */
function parseLine(raw, lineNo) {
  const line = raw.replace(/\r$/, '');
  const tokens = tokenize(line);
  const base = { line: lineNo, raw: line, tokens, kind: '', kindIndex: -1, csg: null, csgIndex: -1, subtype: '', subtypeIndex: -1, positional: [], kwargs: {} };

  if (!line.trim()) return Object.assign(base, { kind: 'blank' });
  if (line[0] === '#') return Object.assign(base, { kind: 'comment' });
  if (/^(OZONE|brushes)\b/.test(line)) return Object.assign(base, { kind: 'header' });
  if (/^[ \t]+#/.test(line) || /^[ \t]*\/\//.test(line)) {
    return Object.assign(base, { kind: 'invalid-comment' });
  }

  let t = 0;
  if (CSG_OPS.has(tokens[0].text)) {
    base.csg = tokens[0].text;
    base.csgIndex = 0;
    t = 1;
  }
  if (!tokens[t]) return Object.assign(base, { kind: 'unknown' });

  base.kind = tokens[t].text;
  base.kindIndex = t;
  if (!BRUSH_KINDS.has(base.kind) && !ENTITY_KINDS.has(base.kind)) {
    base.kind = 'unknown';
    return base;
  }

  let rest = tokens.slice(t + 1);
  if (SUBTYPED.has(base.kind) && rest.length) {
    base.subtype = rest[0].text;
    base.subtypeIndex = tokens.indexOf(rest[0]);
    rest = rest.slice(1);
  }

  for (const tok of rest) {
    const m = /^([A-Za-z_][A-Za-z0-9_.]*)=(.*)$/.exec(tok.text);
    if (m) {
      base.kwargs[m[1]] = { value: m[2], token: tok };
    } else {
      base.positional.push(tok);
    }
  }
  return base;
}

function parseDocument(text) {
  const rawLines = text.split(/\r?\n/);
  return rawLines.map((raw, i) => parseLine(raw, i));
}

function numericTokens(parsed) {
  return parsed.positional.filter((t) => isNumber(t.text));
}

function flagsFrom(parsed) {
  const k = parsed.kwargs.flags;
  if (!k) return null;
  if (!/^\d+$/.test(k.value)) return { raw: k.value, bits: null };
  let n = Number(k.value);
  if (!Number.isFinite(n) || n < 0) return { raw: k.value, bits: null };
  return { raw: k.value, bits: n >>> 0 };
}

function hasKwarg(parsed, name) {
  return Object.prototype.hasOwnProperty.call(parsed.kwargs, name);
}

function hasBare(parsed, word) {
  return parsed.positional.some((t) => t.text === word);
}

function basename(p) {
  if (!p) return '?';
  const clean = p.replace(/^"|"$/g, '');
  const parts = clean.split(/[\\/]/);
  return parts[parts.length - 1] || clean;
}

module.exports = {
  CSG_OPS,
  BRUSH_KINDS,
  ENTITY_KINDS,
  tokenize,
  isNumber,
  parseLine,
  parseDocument,
  numericTokens,
  flagsFrom,
  hasKwarg,
  hasBare,
  basename
};
