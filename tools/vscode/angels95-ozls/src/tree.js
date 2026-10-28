'use strict';

const vscode = require('vscode');
const { getModel } = require('./cache');

const SEVERITY_ICON = {
  error: 'error',
  warning: 'warning',
  info: 'info'
};

function node(label, opts) {
  return Object.assign({
    label,
    description: '',
    tooltip: '',
    icon: 'symbol-field',
    collapsible: vscode.TreeItemCollapsibleState.None,
    children: [],
    command: null
  }, opts || {});
}

function group(label, icon, children, description) {
  return node(label, {
    icon,
    collapsible: vscode.TreeItemCollapsibleState.Expanded,
    children,
    description: description === undefined ? `${children.length}` : description
  });
}

function revealCommand(uri, line) {
  return { command: 'angels95Ozd.reveal', title: 'Reveal', arguments: [uri, line] };
}

function problemNodes(problems, uri) {
  return problems.map((p) => node(p.message, {
    description: `line ${p.line + 1}`,
    icon: SEVERITY_ICON[p.severity] || 'warning',
    tooltip: p.message,
    command: revealCommand(uri, p.line)
  }));
}

function ozlsRoot(document, model) {
  if (!model.entity) {
    return node('No entity header', {
      description: 'expected: entity "Name" : type {',
      icon: 'error',
      tooltip: 'LightningScriptParser returns an empty def when the file does not start with an entity header.'
    });
  }

  const uri = document.uri;
  const children = [];

  if (model.properties.length) {
    children.push(group('Properties', 'symbol-parameter', model.properties.map((p) =>
      node(p.key, {
        description: p.value,
        icon: 'symbol-constant',
        tooltip: `${p.key} = ${p.value}`,
        command: revealCommand(uri, p.line)
      }))));
  }

  if (model.stats) {
    children.push(group('stats', 'symbol-number', model.stats.entries.map((e) =>
      node(e.key, {
        description: e.value,
        icon: e.valueKind === 'vec3' ? 'symbol-array' : e.valueKind === 'string' ? 'symbol-string' : 'symbol-number',
        tooltip: `${e.key} = ${e.value}  (${e.valueKind})`,
        command: revealCommand(uri, e.line)
      }))));
  }

  if (model.actions) {
    const hooks = model.actions.hooks.map((h) => node(h.name, {
      description: `${h.body.length} line${h.body.length === 1 ? '' : 's'}`,
      icon: 'symbol-method',
      tooltip: `${h.name} — action hook`,
      collapsible: h.body.length ? vscode.TreeItemCollapsibleState.Collapsed : vscode.TreeItemCollapsibleState.None,
      children: h.body.map((b) => node(b.text, {
        description: b.opcode,
        icon: 'symbol-event',
        tooltip: b.text,
        command: revealCommand(uri, b.line)
      })),
      command: revealCommand(uri, h.line)
    }));
    children.push(group('actions', 'symbol-method', hooks));
  }

  if (model.variants) {
    const variants = model.variants.variants.map((v) => node(v.name, {
      description: `${v.keys.length} override${v.keys.length === 1 ? '' : 's'}`,
      icon: 'symbol-object',
      collapsible: v.keys.length ? vscode.TreeItemCollapsibleState.Collapsed : vscode.TreeItemCollapsibleState.None,
      children: v.keys.map((k) => node(k.key, {
        description: k.value,
        icon: 'symbol-field',
        tooltip: `${k.key} = ${k.value}`,
        command: revealCommand(uri, k.line)
      })),
      command: revealCommand(uri, v.line)
    }));
    children.push(group('variants', 'symbol-object', variants));
  }

  if (model.problems.length) {
    children.push(group('Problems', 'warning', problemNodes(model.problems, uri)));
  }

  return node(`"${model.entity.name}" : ${model.entity.type}`, {
    description: vscode.workspace.asRelativePath(uri),
    icon: 'symbol-class',
    tooltip: `${model.entity.name} (${model.entity.type})`,
    collapsible: vscode.TreeItemCollapsibleState.Expanded,
    children,
    command: revealCommand(uri, model.entity.line)
  });
}

function pawnRoot(document, model) {
  const uri = document.uri;
  const children = [];

  if (model.root.entries.length) {
    children.push(group('(root)', 'symbol-key', model.root.entries.map((e) =>
      node(e.key, {
        description: e.value,
        icon: 'symbol-field',
        tooltip: `${e.key}=${e.value}`,
        command: revealCommand(uri, e.line)
      }))));
  }

  for (const section of model.sections) {
    children.push(group(section.name, 'symbol-namespace', section.entries.map((e) =>
      node(e.key, {
        description: e.value,
        icon: 'symbol-field',
        tooltip: `${e.key}=${e.value}`,
        command: revealCommand(uri, e.line)
      }))));
  }

  if (model.problems.length) {
    children.push(group('Problems', 'warning', problemNodes(model.problems, uri)));
  }

  return node(vscode.workspace.asRelativePath(uri), {
    description: 'pawn def',
    icon: 'symbol-class',
    collapsible: vscode.TreeItemCollapsibleState.Expanded,
    children
  });
}

class StructureProvider {
  constructor() {
    this._onDidChangeTreeData = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    this.index = null;
    this.pawnIndex = null;
  }

  refresh() {
    this._onDidChangeTreeData.fire();
  }

  invalidateWorkspaceIndex() {
    this.index = null;
    this.pawnIndex = null;
  }

  async ensureWorkspaceIndex() {
    if (this.index) return this.index;
    const files = await vscode.workspace.findFiles(
      '**/*.ozls',
      '**/{.git,node_modules,System,build,build-ed}/**'
    );
    const entries = [];
    await Promise.all(files.map(async (uri) => {
      try {
        const bytes = await vscode.workspace.fs.readFile(uri);
        const text = Buffer.from(bytes).toString('utf8');
        const m = /^\s*entity\s+"([^"]*)"\s*:\s*([A-Za-z0-9_.]+)/m.exec(text);
        if (!m) return;
        const line = text.slice(0, m.index).split(/\r?\n/).length - 1;
        entries.push({
          name: m[1],
          type: m[2],
          uri,
          line,
          rel: vscode.workspace.asRelativePath(uri)
        });
      } catch {
        /* unreadable file: skip it */
      }
    }));
    entries.sort((a, b) => a.type.localeCompare(b.type) || a.name.localeCompare(b.name));
    this.index = entries;
    return entries;
  }

  workspaceRoot(entries) {
    const byType = new Map();
    for (const e of entries) {
      if (!byType.has(e.type)) byType.set(e.type, []);
      byType.get(e.type).push(e);
    }
    const groups = [...byType.entries()].map(([type, list]) =>
      group(type, 'symbol-class', list.map((e) =>
        node(e.name, {
          description: e.rel,
          icon: 'symbol-class',
          tooltip: `${e.name} — ${e.rel}:${e.line + 1}`,
          command: { command: 'angels95Ozd.openWorkspaceDef', title: 'Open', arguments: [e.uri, e.line] }
        }))));
    return node('Workspace Defs', {
      description: `${entries.length} .ozls`,
      icon: 'library',
      collapsible: vscode.TreeItemCollapsibleState.Collapsed,
      children: groups
    });
  }

  async ensurePawnIndex() {
    if (this.pawnIndex) return this.pawnIndex;
    const files = await vscode.workspace.findFiles(
      '**/PawnDefs/*.cfg',
      '**/{.git,node_modules,System,build,build-ed}/**'
    );
    const entries = [];
    await Promise.all(files.map(async (uri) => {
      try {
        const bytes = await vscode.workspace.fs.readFile(uri);
        const text = Buffer.from(bytes).toString('utf8');
        const m = /^\s*name\s*=\s*(.*)$/m.exec(text);
        const line = m ? text.slice(0, m.index).split(/\r?\n/).length - 1 : 0;
        entries.push({
          name: m ? m[1].trim() : vscode.workspace.asRelativePath(uri),
          uri,
          line,
          rel: vscode.workspace.asRelativePath(uri)
        });
      } catch {
        /* unreadable file: skip it */
      }
    }));
    entries.sort((a, b) => a.name.localeCompare(b.name));
    this.pawnIndex = entries;
    return entries;
  }

  pawnRoot(entries) {
    return node('Pawn Defs', {
      description: `${entries.length} .cfg`,
      icon: 'library',
      collapsible: vscode.TreeItemCollapsibleState.Collapsed,
      children: entries.map((e) =>
        node(e.name, {
          description: e.rel,
          icon: 'symbol-class',
          tooltip: `${e.name} — ${e.rel}:${e.line + 1}`,
          command: { command: 'angels95Ozd.openWorkspaceDef', title: 'Open', arguments: [e.uri, e.line] }
        }))
    });
  }

  getTreeItem(element) {
    const item = new vscode.TreeItem(
      element.label,
      element.collapsible === undefined ? vscode.TreeItemCollapsibleState.None : element.collapsible
    );
    item.description = element.description;
    item.tooltip = element.tooltip || element.description;
    item.iconPath = element.icon ? new vscode.ThemeIcon(element.icon) : undefined;
    item.command = element.command || undefined;
    return item;
  }

  async getChildren(element) {
    if (element) return element.children || [];

    const roots = [];
    const editor = vscode.window.activeTextEditor;
    if (editor && (editor.document.languageId === 'ozls' || editor.document.languageId === 'ozpawn')) {
      const model = getModel(editor.document);
      roots.push(model.kind === 'ozpawn'
        ? pawnRoot(editor.document, model)
        : ozlsRoot(editor.document, model));
    } else {
      roots.push(node('Open a .ozls or pawn .cfg file', {
        description: 'the tree follows the active editor',
        icon: 'info'
      }));
    }

    const entries = await this.ensureWorkspaceIndex();
    if (entries.length) roots.push(this.workspaceRoot(entries));

    const pawns = await this.ensurePawnIndex();
    if (pawns.length) roots.push(this.pawnRoot(pawns));
    return roots;
  }
}

module.exports = { StructureProvider };
