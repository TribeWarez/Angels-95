'use strict';

/*
 * Headless smoke test for both extensions. It stubs the `vscode` module so the
 * pure parsing/symbol logic can be exercised without launching an editor.
 *
 *   node tools/vscode/test/smoke.js
 */

const fs = require('fs');
const path = require('path');
const Module = require('module');

/* ------------------------------------------------------------- vscode stub */

class Position {
  constructor(line, character) { this.line = line; this.character = character; }
}
class Range {
  constructor(a, b, c, d) {
    if (a instanceof Position) { this.start = a; this.end = b; }
    else { this.start = new Position(a, b); this.end = new Position(c, d); }
  }
}
class Selection extends Range {}
class DocumentSymbol {
  constructor(name, detail, kind, range, selectionRange, children) {
    this.name = name; this.detail = detail; this.kind = kind;
    this.range = range; this.selectionRange = selectionRange;
    if (children) this.children = children;
  }
}
class Diagnostic {
  constructor(range, message, severity) {
    this.range = range; this.message = message; this.severity = severity;
  }
}
class CodeLens {
  constructor(range, command) { this.range = range; this.command = command; }
}
class Hover {
  constructor(contents, range) { this.contents = contents; this.range = range; }
}
class MarkdownString {
  constructor(value) { this.value = value || ''; }
}
class ThemeIcon {
  constructor(id) { this.id = id; }
}
class EventEmitter {
  constructor() { this.event = () => ({ dispose() {} }); }
  fire() {}
  dispose() {}
}
class TreeItem {
  constructor(label, collapsible) { this.label = label; this.collapsibleState = collapsible; }
}

const stub = {
  Position,
  Range,
  Selection,
  DocumentSymbol,
  Diagnostic,
  CodeLens,
  Hover,
  MarkdownString,
  ThemeIcon,
  TreeItem,
  EventEmitter,
  SymbolKind: {
    File: 0, Module: 1, Namespace: 2, Package: 3, Class: 4, Method: 5, Property: 6,
    Field: 7, Constructor: 8, Enum: 9, Interface: 10, Function: 11, Variable: 12,
    Constant: 13, String: 14, Number: 15, Boolean: 16, Array: 17, Object: 18,
    Key: 19, Null: 20, EnumMember: 21, Struct: 22, Event: 23, Operator: 24,
    TypeParameter: 25
  },
  DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
  TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
  Uri: {
    file: (p) => ({ scheme: 'file', fsPath: p, toString: () => 'file:///' + p.replace(/\\/g, '/') }),
    parse: (s) => ({ scheme: 'file', fsPath: s, toString: () => String(s) })
  },
  workspace: {
    asRelativePath: (p) => String(p && p.fsPath ? p.fsPath : p),
    findFiles: async () => [],
    fs: { readFile: async () => new Uint8Array() },
    getConfiguration: () => ({
      get: (key, def) => (key === 'outline.grouped' ? stub.outlineGrouped === true : def)
    }),
    textDocuments: [],
    openTextDocument: async () => { throw new Error('not available in the smoke test'); },
    createFileSystemWatcher: () => ({ onDidCreate: () => ({ dispose() {} }), onDidDelete: () => ({ dispose() {} }), onDidChange: () => ({ dispose() {} }), dispose() {} }),
    onDidOpenTextDocument: () => ({ dispose() {} }),
    onDidChangeTextDocument: () => ({ dispose() {} }),
    onDidCloseTextDocument: () => ({ dispose() {} }),
    onDidSaveTextDocument: () => ({ dispose() {} })
  },
  window: {
    activeTextEditor: undefined,
    createTreeView: () => ({ dispose() {} }),
    onDidChangeActiveTextEditor: () => ({ dispose() {} }),
    showTextDocument: async () => ({})
  },
  languages: {
    createDiagnosticCollection: () => ({ set() {}, delete() {}, dispose() {} }),
    registerDocumentSymbolProvider: () => ({ dispose() {} }),
    registerHoverProvider: () => ({ dispose() {} }),
    registerCodeLensProvider: () => ({ dispose() {} })
  },
  commands: { registerCommand: () => ({ dispose() {} }) }
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return stub;
  return originalLoad.call(this, request, parent, isMain);
};

/* ----------------------------------------------------------------- helpers */

const ROOT = path.resolve(__dirname, '..', '..', '..');
let failures = 0;
let checks = 0;

function check(condition, message) {
  checks++;
  if (!condition) {
    failures++;
    console.error('FAIL: ' + message);
  }
}

function fakeDocument(text, languageId, name) {
  const lines = text.split(/\r?\n/);
  return {
    uri: stub.Uri.file(name || 'memory://' + languageId),
    languageId,
    version: 1,
    lineCount: lines.length,
    isClosed: false,
    getText: (range) => {
      if (!range) return text;
      if (range.start.line === range.end.line) {
        return (lines[range.start.line] || '').slice(range.start.character, range.end.character);
      }
      return lines.slice(range.start.line, range.end.line + 1).join('\n');
    },
    lineAt: (line) => ({
      text: lines[line] || '',
      range: new Range(line, 0, line, (lines[line] || '').length)
    }),
    getWordRangeAtPosition: (position, pattern) => {
      const line = lines[position.line] || '';
      const re = new RegExp(pattern.source, 'g');
      let m;
      while ((m = re.exec(line)) !== null) {
        if (m.index <= position.character && position.character <= m.index + m[0].length) {
          return new Range(position.line, m.index, position.line, m.index + m[0].length);
        }
        if (m.index > position.character) break;
      }
      return undefined;
    }
  };
}

function read(rel) {
  return fs.readFileSync(path.join(ROOT, rel), 'utf8');
}

function flatten(symbols, out) {
  out = out || [];
  for (const s of symbols) {
    out.push(s);
    if (s.children) flatten(s.children, out);
  }
  return out;
}

function checkRanges(symbols, label) {
  for (const s of symbols) {
    if (!s.children) continue;
    for (const c of s.children) {
      check(c.range.start.line >= s.range.start.line && c.range.end.line <= s.range.end.line,
        `${label}: child '${c.name}' range is outside parent '${s.name}'`);
    }
    checkRanges(s.children, label);
  }
}

/* ------------------------------------------------------------------ grammars */

function testGrammars() {
  const grammarFiles = [
    '../angels95-ozone/syntaxes/ozone.tmLanguage.json',
    '../angels95-ozls/syntaxes/ozls.tmLanguage.json',
    '../angels95-ozls/syntaxes/ozpawn.tmLanguage.json'
  ];

  let compiled = 0;

  function walk(node, where) {
    if (!node || typeof node !== 'object') return;
    if (Array.isArray(node)) {
      node.forEach((v, i) => walk(v, `${where}[${i}]`));
      return;
    }
    for (const key of Object.keys(node)) {
      const value = node[key];
      if ((key === 'match' || key === 'begin' || key === 'end') && typeof value === 'string') {
        // Oniguruma's (?i:...) group has no JS equivalent; translate for the check.
        const js = value.replace(/\(\?i:/g, '(?:');
        try {
          new RegExp(js);
          compiled++;
        } catch (err) {
          check(false, `${where}.${key} is not a valid regex: ${err.message}`);
        }
      } else {
        walk(value, `${where}.${key}`);
      }
    }
  }

  for (const rel of grammarFiles) {
    const grammar = JSON.parse(fs.readFileSync(path.join(__dirname, rel), 'utf8'));
    check(!!grammar.scopeName, `${rel} must declare a scopeName`);
    check(!!grammar.patterns, `${rel} must declare patterns`);
    walk(grammar.repository, rel);

    if (grammar.scopeName === 'source.ozls') {
      // The nested-region structure is load-bearing: a top-level opcode rule
      // would colour stat keys such as `damage`/`heal` as opcodes.
      check(!JSON.stringify(grammar.patterns).includes('#opcode'),
        'opcodes must not be a top-level ozls pattern');
      check(!JSON.stringify(grammar.repository['stats-block'].patterns).includes('#opcode'),
        'the stats region must not include opcodes');
      check(JSON.stringify(grammar.repository['script-line'].patterns).includes('#opcode'),
        'action bodies must include opcodes');
      check(!!grammar.repository['actions-block'] && !!grammar.repository['hook-block'],
        'actions/hook regions must exist');
    }
  }
  check(compiled > 50, `expected to compile the grammar regexes, only saw ${compiled}`);
}

/* -------------------------------------------------------------------- tree */

async function testTree() {
  const { StructureProvider } = require('../angels95-ozls/src/tree.js');
  const provider = new StructureProvider();

  const text = read('GameData/Tex/Global/Gun/Pistol_01/Pistol_01.ozls');
  const document = fakeDocument(text, 'ozls', 'Pistol_01.ozls');
  stub.window.activeTextEditor = { document };

  const roots = await provider.getChildren();
  check(roots.length >= 1, 'tree should expose the active document root');
  check(roots.some((r) => String(r.label).indexOf('pistol_01') >= 0), 'tree root should name the entity');

  for (const root of roots) {
    provider.getTreeItem(root);
    for (const child of root.children || []) {
      provider.getTreeItem(child);
      for (const grandchild of child.children || []) provider.getTreeItem(grandchild);
    }
  }

  stub.window.activeTextEditor = undefined;
}

/* ------------------------------------------------------------------- ozone */

function testOzone() {
  const ozone = require('../angels95-ozone/src/extension.js');
  const { parseDocument } = require('../angels95-ozone/src/parser.js');

  const text = read('GameData/Worlds/TestMap/World.ozone');
  const doc = fakeDocument(text, 'ozone', 'TestMap/World.ozone');

  const parsed = parseDocument(text);
  const boxes = parsed.filter((l) => l.kind === 'box').length;
  check(boxes === 230, `TestMap should parse 230 box brushes, got ${boxes}`);
  check(parsed.filter((l) => l.kind === 'unknown').length === 0, 'TestMap must have no unknown primitives');

  const symbols = ozone._test.buildSymbols(doc);
  check(symbols.length >= 3, 'TestMap outline should have World + geometry + entity groups');
  checkRanges(symbols, 'ozone');
  const names = flatten(symbols).map((s) => s.name);
  check(names.some((n) => /^(?:add |sub |intersect |deresc )?box \(/.test(n)), 'flat outline should contain individual box brushes');
  check(names.filter((n) => /^(?:add |sub |intersect |deresc )?box \(/.test(n)).length === 230, 'flat outline should contain all 230 boxes');
  check(names.some((n) => n.includes('Level Info')), 'outline should contain Level Info');
  check(names.some((n) => n.includes('Player Start')), 'outline should contain Player Start');

  // The opt-in grouped mode still works.
  stub.outlineGrouped = true;
  const groupedSymbols = ozone._test.buildSymbols(doc);
  checkRanges(groupedSymbols, 'ozone grouped');
  const groupedNames = flatten(groupedSymbols).map((s) => s.name);
  check(groupedNames.some((n) => n.startsWith('Brushes: box')), 'grouped outline should group box brushes');
  check(groupedNames.some((n) => n === 'Level Info'), 'grouped outline should contain Level Info');
  stub.outlineGrouped = false;

  const diags = ozone._test.validate(doc);
  check(diags.length === 0, `TestMap should produce no ozone diagnostics, got ${diags.length}`);

  // flags= hover decodes the bitmask
  const flagLine = parsed.find((l) => l.kwargs && l.kwargs.flags);
  const flagSpan = flagLine.tokens.find((t) => t.text === 'flags=16');
  const hover = ozone._test.hoverFor(flagLine, flagSpan);
  check(!!hover, 'flags= hover should return content');
  check(hover && hover.value.indexOf('SURF_COLLISION_PROXY') >= 0, 'flags=16 hover should name SURF_COLLISION_PROXY');

  // Deliberate authoring traps
  const bad = [
    '// not a real comment',
    'portal TargetWorld 0 0 0',
    'add box 0 0 0 1 1 1 0 flags=0x8',
    'add box 0 0 0 1 1 1 0 surfAlpha=0.5',
    'add box 0 0 0 1 1 1 0 flags=16',
    'pickup rifle_01 0 0 1 respawn=30',
    'light point 0 0 3 255 255 255 1',
    'trigger MyVol 0 0 0 2 2 2 once'
  ].join('\n');
  const badDoc = fakeDocument(bad, 'ozone', 'bad.ozone');
  const badDiags = ozone._test.validate(badDoc);
  const messages = badDiags.map((d) => d.message).join('\n');
  check(/Not a comment/.test(messages), 'a // line should be reported as not-a-comment');
  check(/6 box numbers/.test(messages), 'a short portal should be reported');
  check(/not a decimal integer/.test(messages), 'flags=0x8 should be reported');
  check(/respawn=` is not parsed|respawn= is not parsed/.test(messages), 'respawn= should be reported');
  check(/radius 0/.test(messages), 'a point light without a radius should be reported');

  // Deprecated surface kwargs are scoped + hovered, not turned into Problems entries.
  const deprecated = parseDocument('add box 0 0 0 1 1 1 0 surfAlpha=0.5')[0];
  const deprecatedSpan = deprecated.tokens.find((t) => t.text === 'surfAlpha=0.5');
  const deprecatedHover = ozone._test.hoverFor(deprecated, deprecatedSpan);
  check(deprecatedHover && /not parsed by the engine/.test(deprecatedHover.value),
    'surfAlpha= hover should say it is not parsed');

  console.log(`ozone: ${checks - failures}/${checks} checks passed so far`);
}

/* -------------------------------------------------------------------- ozls */

function testOzls() {
  const ozls = require('../angels95-ozls/src/extension.js');
  const assist = require('../angels95-ozls/src/assist.js');

  const text = read('GameData/Tex/Global/Gun/Pistol_01/Pistol_01.ozls');
  const doc = fakeDocument(text, 'ozls', 'Pistol_01.ozls');
  const model = ozls._test.getModel(doc);

  check(model.entity && model.entity.name === 'pistol_01', 'entity name should parse');
  check(model.entity && model.entity.type === 'weapon', 'entity type should parse');
  check(model.entity && model.entity.validType, 'weapon should be a known type');
  check(model.stats && model.stats.entries.length > 10, 'weapon stats should parse');
  check(model.actions && model.actions.hooks.length === 2, 'weapon should expose 2 action hooks');
  check(model.problems.length === 0, `Pistol_01.ozls should be clean, got ${model.problems.length}`);

  const symbols = ozls._test.buildSymbolsFor(doc);
  checkRanges(symbols, 'ozls');
  const flat = flatten(symbols);
  check(flat.some((s) => s.name === 'stats'), 'outline should contain a stats node');
  check(flat.some((s) => s.name === 'on_fire'), 'outline should contain on_fire');
  check(flat.some((s) => s.name === 'damage'), 'outline should contain the damage stat');

  // Hover + CodeLens
  const lines = text.split(/\r?\n/);
  const damageLine = model.stats.entries.find((e) => e.key === 'damage').line;
  const damageHover = assist.provideHover(doc, new Position(damageLine, lines[damageLine].indexOf('damage') + 2));
  check(damageHover && /Damage per hit/.test(damageHover.contents.value),
    'hovering a stat key should explain it');

  const hookLine = model.actions.hooks[0].line;
  const hookHover = assist.provideHover(doc, new Position(hookLine, lines[hookLine].indexOf('on_fire') + 2));
  check(hookHover && /ranged shot/.test(hookHover.contents.value),
    'hovering on_fire should explain the hook');

  const lenses = assist.provideCodeLenses(doc);
  check(lenses.some((l) => /^\d+ stats$/.test(l.command.title)), 'code lens should show the stat count');
  check(lenses.some((l) => /^\d+ actions$/.test(l.command.title)), 'code lens should show the action count');
  check(lenses.some((l) => /^\d+ lines?$/.test(l.command.title)), 'code lens should show per-hook line counts');

  // The vec3 trap: `vec3 colour = (1,2,3)` aborts the block.
  const trap = [
    'entity "Trap" : weapon {',
    '    stats {',
    '        vec3 color = (1, 2, 3)',
    '        damage = 5',
    '    }',
    '    actions {',
    '        on_world.load {',
    '            say "never fires"',
    '        }',
    '    }',
    '}'
  ].join('\n');
  const trapModel = ozls._test.getModel(fakeDocument(trap, 'ozls', 'trap.ozls'));
  const trapMessages = trapModel.problems.map((p) => p.message).join('\n');
  check(/vec3.*not valid/i.test(trapMessages), 'a vec3 prefix should be reported');
  check(/can NEVER fire/.test(trapMessages), 'an action name with a dot should be reported');

  // Unknown top-level key (the historical model_height bug).
  const unknown = 'entity "X" : upgrade {\n    model_height = 3.0\n}\n';
  const unknownModel = ozls._test.getModel(fakeDocument(unknown, 'ozls', 'unknown.ozls'));
  check(unknownModel.problems.some((p) => /Unknown top-level key 'model_height'/.test(p.message)),
    'model_height outside stats should be reported');

  // A stat value with a space is truncated by the parser.
  const spaced = 'entity "X" : weapon {\n    stats {\n        fire_sound = "my gun.wav"\n    }\n}\n';
  const spacedModel = ozls._test.getModel(fakeDocument(spaced, 'ozls', 'spaced.ozls'));
  check(spacedModel.problems.some((p) => /containing a space/.test(p.message)),
    'a spaced stat value should be reported');

  // Pawn defs
  const pawn = read('GameData/Pawn/PawnDefs/Walker.cfg');
  const pawnDoc = fakeDocument(pawn, 'ozpawn', 'Walker.cfg');
  const pawnModel = ozls._test.getModel(pawnDoc);
  check(pawnModel.root.entries.length > 15, 'Walker.cfg should expose its keys');
  check(pawnModel.problems.length === 0, 'Walker.cfg should be clean');

  const pawnLines = pawn.split(/\r?\n/);
  const speedLine = pawnLines.findIndex((l) => /^speed\s*=/.test(l));
  const pawnHover = assist.provideHover(pawnDoc, new Position(speedLine, 1));
  check(pawnHover && /speed=/.test(pawnHover.contents.value),
    'hovering a pawn-def key should explain it');

  const badPawn = 'name=Broken\nspeed=fast\nmaxHealth=10.5\n';
  const badPawnModel = ozls._test.getModel(fakeDocument(badPawn, 'ozpawn', 'Broken.cfg'));
  const badPawnMessages = badPawnModel.problems.map((p) => p.message).join('\n');
  check(/speed = fast.*not a number/.test(badPawnMessages), 'a non-numeric speed should be reported (client crash)');
  check(/must be an integer/.test(badPawnMessages), 'a fractional maxHealth should be reported');

  console.log(`all: ${checks - failures}/${checks} checks passed`);
}

(async () => {
  try {
    testGrammars();
    testOzone();
    testOzls();
    await testTree();
  } catch (err) {
    failures++;
    console.error(err && err.stack ? err.stack : err);
  }

  if (failures) {
    console.error(`\n${failures} check(s) failed.`);
    process.exit(1);
  }
  console.log('\nOK');
})();
