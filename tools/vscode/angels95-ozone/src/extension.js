'use strict';

const vscode = require('vscode');
const { PRIMITIVES, ENTITIES, ZONE_TYPES, LIGHT_TYPES, KWARGS, FLAG_BITS } = require('./data');
const P = require('./parser');

const LANGUAGE = 'ozone';

const LIGHT_ARGS = {
  point: [
    'X position', 'Y position', 'Z position',
    'red (0-255)', 'green (0-255)', 'blue (0-255)',
    'intensity', 'radius — a point light with no radius is invisible',
    'effect (0 none, 1 watery, 2 torch, 3 fire, 4 lamp)',
    'flare (0/1)', 'corona (0/1)'
  ],
  spot: [
    'X position', 'Y position', 'Z position',
    'target X', 'target Y', 'target Z',
    'red', 'green', 'blue', 'intensity', 'radius',
    'inner cone — a COSINE', 'outer cone — a COSINE',
    'effect', 'flare', 'corona'
  ],
  directional: [
    'X position — the light SOURCE, aimed at the world origin',
    'Y position', 'Z position',
    'red', 'green', 'blue', 'intensity',
    'flare (0/1)', 'corona (0/1)'
  ]
};

const ZONE_ARGS = [
  'min X', 'min Y', 'min Z', 'max X', 'max Y', 'max Z',
  'intensity',
  'fog red', 'fog green', 'fog blue', 'fog density', 'fog start', 'fog end',
  'ambient red', 'ambient green', 'ambient blue', 'ambient intensity',
  'reverb mix', 'reverb decay'
];

const DEPRECATED_KWARGS = {
  surfAlpha: 'alpha',
  surfCutoff: 'cutoff',
  surfGlowScale: 'glowScale'
};

const GAMETYPE_NAMES = [
  'singleplayer', 'coop', 'etheral_match', 'angel_team_game', 'angel_run',
  'capture_the_orb', 'time_shift', 'deathmatch', 'ctf', 'campaign', 'cooperative'
];

const cache = new WeakMap();
const timers = new Map();
let diagnostics;

function getParsed(document) {
  const hit = cache.get(document);
  if (hit && hit.version === document.version) return hit.parsed;
  const parsed = P.parseDocument(document.getText());
  cache.set(document, { version: document.version, parsed });
  return parsed;
}

function lineRange(parsed) {
  return new vscode.Range(parsed.line, 0, parsed.line, parsed.raw.length);
}

function spanRange(parsed, span) {
  return new vscode.Range(parsed.line, span.start, parsed.line, span.end);
}

/* ------------------------------------------------------------------ symbols */

function groupSymbol(name, kind, members) {
  const first = members[0].parsed;
  const last = members[members.length - 1].parsed;
  return new vscode.DocumentSymbol(
    name,
    `${members.length}`,
    kind,
    new vscode.Range(first.line, 0, last.line, last.raw.length),
    lineRange(first),
    members.map((m) => m.symbol)
  );
}

function brushSymbol(parsed) {
  const nums = parsed.positional.filter((t) => P.isNumber(t.text));
  const where = nums.length >= 3 ? `(${nums[0].text}, ${nums[1].text}, ${nums[2].text})` : '';
  const name = `${parsed.csg ? parsed.csg + ' ' : ''}${parsed.kind} ${where}`.trim();
  return new vscode.DocumentSymbol(
    name,
    parsed.csg || 'add',
    vscode.SymbolKind.Struct,
    lineRange(parsed),
    lineRange(parsed)
  );
}

function entitySymbol(parsed, ordinal) {
  const pos = parsed.positional;
  const name = (() => {
    switch (parsed.kind) {
      case 'levelinfo': return 'Level Info';
      case 'playerstart': return 'Player Start';
      case 'pickup': return `pickup ${pos[0] ? pos[0].text : '?'}`;
      case 'npc': return `npc ${pos[0] ? pos[0].text : '?'}`;
      case 'zone': {
        const n = parsed.kwargs.name ? parsed.kwargs.name.value : parsed.subtype;
        return `zone ${parsed.subtype}${n ? ' — ' + n : ''}`;
      }
      case 'light': {
        const n = parsed.kwargs.name ? ` — ${parsed.kwargs.name.value}` : '';
        return `light ${parsed.subtype}${n}`;
      }
      case 'portal': return `portal → ${pos[0] ? pos[0].text : '?'}`;
      case 'trigger': return `trigger ${pos[0] ? pos[0].text : '?'}`;
      case 'Mesh.Static':
      case 'Mesh.Skeletal': return `${parsed.kind} ${P.basename(pos[0] && pos[0].text)}`;
      case 'ParticleEmitter': return `ParticleEmitter ${pos[0] ? pos[0].text : '?'}`;
      case 'PathNode': return `PathNode ${pos[0] ? pos[0].text : '?'}`;
      case 'WindZone': return 'WindZone';
      case 'door': return `door ${P.basename(pos[0] && pos[0].text)}`;
      case 'emitter': return `emitter ${pos[0] ? pos[0].text : '?'}`;
      case 'particles': return `particles ${pos[0] ? pos[0].text : '?'}`;
      case 'skybox': return `skybox ${P.basename(pos[0] && pos[0].text)}`;
      case 'heightmap': return `heightmap ${P.basename(pos[1] && pos[1].text)}`;
      default: return `${parsed.kind} ${ordinal + 1}`;
    }
  })();

  return new vscode.DocumentSymbol(
    name,
    parsed.kind,
    vscode.SymbolKind.Object,
    lineRange(parsed),
    lineRange(parsed)
  );
}

function buildSymbols(document) {
  const parsed = getParsed(document);
  const roots = [];
  const flat = [];

  const world = [];
  const brushes = new Map();
  const entities = new Map();

  for (const p of parsed) {
    if (p.kind === 'blank' || p.kind === 'comment' || p.kind === 'header' || p.kind === 'invalid-comment' || p.kind === 'unknown') continue;
    if (p.kind === 'levelinfo' || p.kind === 'particles' || p.kind === 'skybox' || p.kind === 'heightmap') {
      const entry = { parsed: p, symbol: entitySymbol(p, world.length) };
      world.push(entry);
      flat.push(entry.symbol);
      continue;
    }
    if (P.BRUSH_KINDS.has(p.kind)) {
      if (!brushes.has(p.kind)) brushes.set(p.kind, []);
      const entry = { parsed: p, symbol: brushSymbol(p) };
      brushes.get(p.kind).push(entry);
      flat.push(entry.symbol);
      continue;
    }
    if (!entities.has(p.kind)) entities.set(p.kind, []);
    const entry = { parsed: p, symbol: entitySymbol(p, entities.get(p.kind).length) };
    entities.get(p.kind).push(entry);
    flat.push(entry.symbol);
  }

  const grouped = vscode.workspace.getConfiguration('angels95Ozone').get('outline.grouped', false);
  if (!grouped) return flat;

  if (world.length) {
    roots.push(groupSymbol('World', vscode.SymbolKind.Namespace, world));
  }

  const brushOrder = ['box', 'cyl', 'sph', 'pyr', 'pln'];
  for (const kind of brushOrder) {
    const list = brushes.get(kind);
    if (list && list.length) {
      roots.push(groupSymbol(`Brushes: ${kind}`, vscode.SymbolKind.Namespace, list));
    }
  }

  const entityOrder = [
    'playerstart', 'pickup', 'npc', 'light', 'zone', 'portal', 'trigger',
    'emitter', 'Mesh.Static', 'Mesh.Skeletal', 'ParticleEmitter', 'PathNode',
    'WindZone', 'door'
  ];
  for (const kind of entityOrder) {
    const list = entities.get(kind);
    if (list && list.length) {
      roots.push(groupSymbol(`${kind} (${list.length})`, vscode.SymbolKind.Namespace, list));
    }
  }

  return roots;
}

/* -------------------------------------------------------------------- hover */

function md(lines) {
  return new vscode.MarkdownString(lines.filter(Boolean).join('\n\n'));
}

function code(s) {
  return '`' + s + '`';
}

function decodeFlags(bits) {
  if (bits === 0) return ['No surface flags set.'];
  const rows = [];
  let seen = 0;
  for (const [bit, name, doc] of FLAG_BITS) {
    if (bits & bit) {
      rows.push(`- ${code(name)} (${bit}) — ${doc}`);
      seen |= bit;
    }
  }
  const unknown = (bits & ~seen) >>> 0;
  if (unknown) rows.push(`- **Unknown bits:** ${code(String(unknown))}`);
  return rows;
}

function signatureFor(parsed) {
  if (parsed.kind === 'zone') return ENTITIES.zone.sig;
  if (parsed.kind === 'light') return ENTITIES.light.sig;
  if (PRIMITIVES[parsed.kind]) return PRIMITIVES[parsed.kind].sig;
  if (ENTITIES[parsed.kind]) return ENTITIES[parsed.kind].sig;
  return null;
}

function argDocsFor(parsed) {
  if (parsed.kind === 'zone') return ZONE_ARGS;
  if (parsed.kind === 'light') return LIGHT_ARGS[parsed.subtype] || [];
  if (PRIMITIVES[parsed.kind]) return PRIMITIVES[parsed.kind].args;
  if (ENTITIES[parsed.kind]) return ENTITIES[parsed.kind].args;
  return [];
}

function hoverFor(parsed, span) {
  const text = span.text;

  // flags=N decoding
  let m = /^flags=(\d+)$/.exec(text);
  if (m) {
    const bits = Number(m[1]) >>> 0;
    return md([
      `**flags=${m[1]}**`,
      decodeFlags(bits).join('\n'),
      '_Hover `flags=` on any brush line to decode the mask._'
    ]);
  }

  // face<name>_<field>=
  m = /^(face(px|nx|py|ny|pz|nz)_[A-Za-z]+)=(.*)$/.exec(text);
  if (m) {
    return md([
      `**${m[1]}** — per-face surface override`,
      `Face ${code(m[2])}: ${faceDescription(m[2])}.`,
      'A face override starts from the brush-wide default, so it only needs to state the difference.'
    ]);
  }

  // key=value kwarg
  m = /^([A-Za-z_][A-Za-z0-9_.]*)=(.*)$/.exec(text);
  if (m) {
    const key = m[1];
    if (DEPRECATED_KWARGS[key]) {
      return md([
        `**${key}=** — **not parsed by the engine**`,
        `\`OzoneParser::ParseSurfaceField\` does not read ${code(key + '=')}. Use ${code(DEPRECATED_KWARGS[key] + '=')} instead.`,
        'The AngelEd exporter still writes the dead spelling, so opening and saving a level changes nothing.'
      ]);
    }
    if (KWARGS[key]) {
      const extra = key === 'effect'
        ? ['**Enum:** 0 NONE, 1 WATERY, 2 TORCH, 3 FIRE, 4 LAMP.']
        : key === 'gametype'
          ? ['Known keys: ' + GAMETYPE_NAMES.map(code).join(', ') + '.']
          : [];
      return md([`**${key}=** — ${KWARGS[key]}`, ...extra]);
    }
    return md([`**${key}=**`, 'Unknown keyword. Unknown kwargs on a brush line are ignored silently by design; on some entity lines a trailing token silently ends the line.']);
  }

  // csg operator
  if (parsed.csgIndex >= 0 && parsed.tokens[parsed.csgIndex] === span) {
    const doc = {
      add: 'Adds a solid. A bare primitive (`box …`) is also `add`.',
      sub: 'Subtracts from solids that already exist. **Collision only — never the render mesh.** A world whose only brush is `sub` has zero collision volumes.',
      intersect: 'Intersects with solids that already exist. Also collision-only.',
      deresc: 'Alias of `sub`. The exporter always writes `sub`.'
    }[parsed.csg] || '';
    return md([`**${parsed.csg}** — ${doc}`]);
  }

  // primitive / entity keyword
  if (parsed.kindIndex >= 0 && parsed.tokens[parsed.kindIndex] === span) {
    const sig = signatureFor(parsed);
    const doc = (PRIMITIVES[parsed.kind] || ENTITIES[parsed.kind] || {}).doc;
    return md([sig ? '```ozone\n' + sig + '\n```' : null, doc]);
  }

  // zone / light subtype
  if (parsed.subtypeIndex >= 0 && parsed.tokens[parsed.subtypeIndex] === span) {
    const table = parsed.kind === 'zone' ? ZONE_TYPES : LIGHT_TYPES;
    return md([`**${parsed.subtype}** — ${table[parsed.subtype] || 'Unknown subtype.'}`]);
  }

  // positional argument
  const posIndex = parsed.positional.indexOf(span);
  if (posIndex >= 0) {
    const args = argDocsFor(parsed);
    const doc = args[posIndex];
    if (doc) {
      const sig = signatureFor(parsed);
      return md([`**Argument ${posIndex + 1}** — ${doc}`, sig ? '```ozone\n' + sig + '\n```' : null]);
    }
    if (text === 'bidir') {
      return md(['**bidir** — bidirectional portal. Only read from arg 9, so it does nothing unless the spawn triple is present.']);
    }
    if (text === 'once') {
      return md(['**once** — the trigger fires at most once.']);
    }
    if (text === 'loop') {
      return md(['**loop** — the PathNode graph wraps back to its first node.']);
    }
    if (text === 'wind') {
      return md(['**wind** — marks a mesh as wind-affected. Requires an enclosing `WindZone`; without one it silently does nothing.']);
    }
  }

  if (parsed.kind === 'heightmap' && posIndex === 1) {
    return md(['**Terrain texture path**']);
  }

  return null;
}

function faceDescription(name) {
  return {
    px: '+X face (engine Y-up naming)',
    nx: '−X face',
    py: '+Y face',
    ny: '−Y face',
    pz: '+Z face',
    nz: '−Z face'
  }[name] || name;
}

/* -------------------------------------------------------------- diagnostics */

function diag(parsed, message, severity, span) {
  const d = new vscode.Diagnostic(
    span ? spanRange(parsed, span) : lineRange(parsed),
    message,
    severity
  );
  d.source = 'Angels95 OZONE';
  return d;
}

function validate(document) {
  const parsedLines = getParsed(document);
  const out = [];

  for (const p of parsedLines) {
    if (p.kind === 'blank' || p.kind === 'comment' || p.kind === 'header') continue;

    if (p.kind === 'invalid-comment') {
      out.push(diag(
        p,
        'Not a comment. OZONE only treats `#` as a comment when it is the FIRST character of the line; this line is parsed as an unknown primitive, ignored, and deleted by the next editor save.',
        vscode.DiagnosticSeverity.Warning
      ));
      continue;
    }

    if (p.kind === 'unknown') {
      const tok = p.csgIndex >= 0 ? p.tokens[p.csgIndex + 1] : p.tokens[0];
      out.push(diag(
        p,
        `Unknown OZONE primitive '${tok ? tok.text : ''}'. OzoneParser warns and skips the line; the first token is case-sensitive.`,
        vscode.DiagnosticSeverity.Warning,
        tok
      ));
      continue;
    }

    // Dead surface kwargs the exporter still writes are surfaced by the grammar
    // scope and the hover provider, not as Problems entries: they appear 100+
    // times in shipped worlds, and a wall of hints is not an authoring signal.

    // flags= must be a plain decimal int; 0x8 parses as 0.
    if (P.hasKwarg(p, 'flags')) {
      const f = p.kwargs.flags;
      if (!/^\d+$/.test(f.value)) {
        out.push(diag(
          p,
          `flags='${f.value}' is not a decimal integer. OzoneParser uses std::stoi, so '0x8' parses as 0 and a value above 2147483647 is rejected.`,
          vscode.DiagnosticSeverity.Warning,
          f.token
        ));
      }
    }

    const nums = P.numericTokens(p);

    if (p.kind === 'portal') {
      if (nums.length < 6) {
        out.push(diag(
          p,
          `Portal needs 6 box numbers <minX minY minZ maxX maxY maxZ>; got ${nums.length}. The loader drops a short portal with no runtime warning, and AngelEd deletes it on the next save.`,
          vscode.DiagnosticSeverity.Warning
        ));
      } else if (P.hasBare(p, 'bidir') && nums.length < 9) {
        out.push(diag(
          p,
          '`bidir` is read from arg 9, so it does nothing without the spawn triple <spawnX spawnY spawnZ>.',
          vscode.DiagnosticSeverity.Warning
        ));
      }
    }

    if (p.kind === 'trigger' && nums.length < 6) {
      out.push(diag(
        p,
        `Trigger needs 6 box numbers <minX minY minZ maxX maxY maxZ>; got ${nums.length}. It is parsed but never reaches the runtime, and AngelEd deletes it on the next save.`,
        vscode.DiagnosticSeverity.Warning
      ));
    }

    if (p.kind === 'light') {
      const need = p.subtype === 'point' ? 7 : p.subtype === 'spot' ? 12 : p.subtype === 'directional' ? 6 : 0;
      if (need && nums.length < need) {
        out.push(diag(
          p,
          `light ${p.subtype} needs at least ${need} numbers before the optional tail; got ${nums.length}. The loader drops the line.`,
          vscode.DiagnosticSeverity.Warning
        ));
      }
      if (p.subtype === 'point' && nums.length === 7) {
        out.push(diag(
          p,
          'A point light with 7 numbers has radius 0, which renders nothing. Author a radius.',
          vscode.DiagnosticSeverity.Warning
        ));
      }
    }

    if (p.kind === 'pickup' && P.hasKwarg(p, 'respawn')) {
      out.push(diag(
        p,
        '`respawn=` is not parsed. The respawn time is the 4th positional number: `pickup <type> <x> <y> <z> <seconds>`.',
        vscode.DiagnosticSeverity.Warning,
        p.kwargs.respawn.token
      ));
    }

    const minNums = {
      playerstart: 3,
      pickup: 3,
      npc: 3,
      zone: 6,
      WindZone: 10,
      ParticleEmitter: 3,
      PathNode: 3,
      door: 5
    }[p.kind];
    if (minNums && nums.length < minNums) {
      out.push(diag(
        p,
        `${p.kind} needs at least ${minNums} positional numbers; got ${nums.length}. The line is dropped by the loader.`,
        vscode.DiagnosticSeverity.Warning
      ));
    }

    if ((p.kind === 'Mesh.Static' || p.kind === 'Mesh.Skeletal') && p.positional.length < 5) {
      out.push(diag(
        p,
        `${p.kind} needs <path> <x> <y> <z> <yaw>. A missing yaw makes the loader skip the prop.`,
        vscode.DiagnosticSeverity.Warning
      ));
    }
  }

  return out;
}

function scheduleValidation(document) {
  if (document.languageId !== LANGUAGE) return;
  const key = document.uri.toString();
  if (timers.has(key)) clearTimeout(timers.get(key));
  timers.set(key, setTimeout(() => {
    timers.delete(key);
    if (document.isClosed) return;
    diagnostics.set(document.uri, validate(document));
  }, 200));
}

/* ------------------------------------------------------------------- activate */

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection('angels95-ozone');
  context.subscriptions.push(diagnostics);

  context.subscriptions.push(
    vscode.languages.registerDocumentSymbolProvider(
      { language: LANGUAGE },
      { provideDocumentSymbols: (document) => buildSymbols(document) },
      { label: 'OZONE World' }
    )
  );

  context.subscriptions.push(
    vscode.languages.registerHoverProvider(
      { language: LANGUAGE },
      {
        provideHover(document, position) {
          const parsed = getParsed(document);
          const line = parsed[position.line];
          if (!line || !line.tokens.length) return null;
          const span = line.tokens.find((t) => position.character >= t.start && position.character <= t.end)
            || line.tokens.find((t) => position.character >= t.start && position.character < t.end);
          if (!span) return null;
          const content = hoverFor(line, span);
          return content ? new vscode.Hover(content, spanRange(line, span)) : null;
        }
      }
    )
  );

  const openDocs = vscode.workspace.textDocuments || [];
  for (const document of openDocs) {
    if (document.languageId === LANGUAGE) diagnostics.set(document.uri, validate(document));
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(scheduleValidation),
    vscode.workspace.onDidChangeTextDocument((e) => scheduleValidation(e.document)),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      const key = doc.uri.toString();
      if (timers.has(key)) {
        clearTimeout(timers.get(key));
        timers.delete(key);
      }
      diagnostics.delete(doc.uri);
    })
  );
}

function deactivate() {
  if (diagnostics) diagnostics.dispose();
}

module.exports = {
  activate,
  deactivate,
  _test: { validate, buildSymbols, hoverFor, getParsed }
};
