'use strict';

const vscode = require('vscode');

function rangeFor(line, text) {
  return new vscode.Range(line, 0, line, (text || '').length);
}

function symbol(name, detail, kind, line, children, endLine) {
  const start = rangeFor(line);
  const lastLine = endLine === undefined ? line : endLine;
  const end = rangeFor(lastLine);
  const range = new vscode.Range(start.start, end.end);
  const sym = new vscode.DocumentSymbol(name, detail, kind, range, start);
  if (children && children.length) sym.children = children;
  return sym;
}

function truncate(text, max) {
  const t = String(text == null ? '' : text);
  return t.length > max ? t.slice(0, max - 1) + '…' : t;
}

/* --------------------------------------------------------------------- .ozls */

function buildOzlsSymbols(model, document) {
  if (!model.entity) return [];
  const ent = model.entity;
  const lastLine = Math.max(ent.line, document.lineCount - 1);
  const children = [];

  if (model.properties.length) {
    const fields = model.properties.map((p) =>
      symbol(p.key, truncate(p.value, 60), vscode.SymbolKind.Field, p.line));
    children.push(symbol('Properties', `${fields.length}`, vscode.SymbolKind.Object, ent.line, fields,
      model.properties[model.properties.length - 1].line));
  }

  if (model.stats) {
    const fields = model.stats.entries.map((e) =>
      symbol(e.key, truncate(e.value, 60), vscode.SymbolKind.Field, e.line));
    const last = model.stats.entries.length
      ? model.stats.entries[model.stats.entries.length - 1].line
      : model.stats.line;
    children.push(symbol('stats', `${fields.length}`, vscode.SymbolKind.Object, model.stats.line, fields, last));
  }

  if (model.actions) {
    const hooks = model.actions.hooks.map((h) => {
      const endLine = h.body.length ? h.body[h.body.length - 1].line : h.line;
      const body = h.body.map((b) =>
        symbol(truncate(b.text, 80), b.opcode, vscode.SymbolKind.String, b.line));
      return symbol(h.name, `${h.body.length} line${h.body.length === 1 ? '' : 's'}`, vscode.SymbolKind.Method, h.line, body, endLine);
    });
    const last = model.actions.hooks.length
      ? (() => {
        const h = model.actions.hooks[model.actions.hooks.length - 1];
        return h.body.length ? h.body[h.body.length - 1].line : h.line;
      })()
      : model.actions.line;
    children.push(symbol('actions', `${hooks.length}`, vscode.SymbolKind.Object, model.actions.line, hooks, last));
  }

  if (model.variants) {
    const variants = model.variants.variants.map((v) => {
      const keys = v.keys.map((k) => symbol(k.key, truncate(k.value, 60), vscode.SymbolKind.Field, k.line));
      const lastKey = v.keys.length ? v.keys[v.keys.length - 1].line : v.line;
      return symbol(v.name, `${v.keys.length}`, vscode.SymbolKind.Object, v.line, keys, lastKey);
    });
    const last = model.variants.variants.length
      ? (() => {
        const v = model.variants.variants[model.variants.variants.length - 1];
        return v.keys.length ? v.keys[v.keys.length - 1].line : v.line;
      })()
      : model.variants.line;
    children.push(symbol('variants', `${variants.length}`, vscode.SymbolKind.Object, model.variants.line, variants, last));
  }

  if (model.problems.length) {
    const items = model.problems.map((p) =>
      symbol(truncate(p.message, 100), p.severity, vscode.SymbolKind.Event, p.line));
    children.push(symbol('Problems', `${items.length}`, vscode.SymbolKind.Object, ent.line, items,
      model.problems[model.problems.length - 1].line));
  }

  const detail = `${ent.type}${ent.validType ? '' : ' (unknown type)'}`;
  const root = symbol(`"${ent.name}"`, detail, vscode.SymbolKind.Class, ent.line, children, lastLine);
  return [root];
}

/* ---------------------------------------------------------------------- .cfg */

function buildPawnSymbols(model) {
  const roots = [];
  if (model.root.entries.length) {
    const fields = model.root.entries.map((e) =>
      symbol(e.key, truncate(e.value, 60), vscode.SymbolKind.Field, e.line));
    roots.push(symbol('(root)', `${fields.length}`, vscode.SymbolKind.Object, model.root.line, fields,
      model.root.entries[model.root.entries.length - 1].line));
  }
  for (const section of model.sections) {
    const fields = section.entries.map((e) =>
      symbol(e.key, truncate(e.value, 60), vscode.SymbolKind.Field, e.line));
    const last = section.entries.length ? section.entries[section.entries.length - 1].line : section.line;
    roots.push(symbol(section.name || '(unnamed section)', `${fields.length}`, vscode.SymbolKind.Namespace, section.line, fields, last));
  }
  if (model.problems.length) {
    const items = model.problems.map((p) =>
      symbol(truncate(p.message, 100), p.severity, vscode.SymbolKind.Event, p.line));
    roots.push(symbol('Problems', `${items.length}`, vscode.SymbolKind.Object, 0, items,
      model.problems[model.problems.length - 1].line));
  }
  return roots;
}

function buildSymbols(model, document) {
  if (!model) return [];
  return model.kind === 'ozpawn' ? buildPawnSymbols(model) : buildOzlsSymbols(model, document);
}

module.exports = { buildSymbols };
