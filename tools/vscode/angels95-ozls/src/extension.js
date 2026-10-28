'use strict';

const vscode = require('vscode');
const { getModel, invalidate } = require('./cache');
const { buildSymbols } = require('./symbols');
const { collect } = require('./diagnostics');
const { StructureProvider } = require('./tree');
const { provideHover, provideCodeLenses } = require('./assist');

const LANGS = ['ozls', 'ozpawn'];
const VALIDATE_DELAY = 150;

let diagnostics;
let provider;
const timers = new Map();

function isTarget(document) {
  return !!document && LANGS.indexOf(document.languageId) >= 0;
}

function validate(document) {
  if (!isTarget(document)) return;
  diagnostics.set(document.uri, collect(getModel(document)));
}

function schedule(document) {
  if (!isTarget(document)) return;
  const key = document.uri.toString();
  if (timers.has(key)) clearTimeout(timers.get(key));
  timers.set(key, setTimeout(() => {
    timers.delete(key);
    if (document.isClosed) return;
    validate(document);
    if (provider) provider.refresh();
  }, VALIDATE_DELAY));
}

function clearTimer(uri) {
  const key = uri.toString();
  if (timers.has(key)) {
    clearTimeout(timers.get(key));
    timers.delete(key);
  }
}

async function openAt(uriLike, line) {
  const uri = uriLike && uriLike.scheme ? uriLike : vscode.Uri.parse(String(uriLike));
  const document = await vscode.workspace.openTextDocument(uri);
  const editor = await vscode.window.showTextDocument(document, { preview: false });
  const pos = new vscode.Position(Math.max(0, line | 0), 0);
  editor.selection = new vscode.Selection(pos, pos);
  editor.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenterIfOutsideViewport);
}

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection('angels95-ozls');
  provider = new StructureProvider();

  const tree = vscode.window.createTreeView('angels95OzdStructure', {
    treeDataProvider: provider,
    showCollapseAll: true
  });

  context.subscriptions.push(diagnostics, tree);

  context.subscriptions.push(
    vscode.languages.registerDocumentSymbolProvider(
      { language: 'ozls' },
      {
        provideDocumentSymbols(document) {
          return buildSymbols(getModel(document), document);
        }
      },
      { label: 'LightningScript' }
    ),
    vscode.languages.registerDocumentSymbolProvider(
      { language: 'ozpawn' },
      {
        provideDocumentSymbols(document) {
          return buildSymbols(getModel(document), document);
        }
      },
      { label: 'Pawn Def' }
    ),
    vscode.languages.registerHoverProvider(
      { language: 'ozls' },
      { provideHover }
    ),
    vscode.languages.registerHoverProvider(
      { language: 'ozpawn' },
      { provideHover }
    ),
    vscode.languages.registerCodeLensProvider(
      { language: 'ozls' },
      { provideCodeLenses }
    ),
    vscode.languages.registerCodeLensProvider(
      { language: 'ozpawn' },
      { provideCodeLenses }
    )
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('angels95Ozd.refresh', () => {
      if (provider) provider.invalidateWorkspaceIndex();
      for (const document of vscode.workspace.textDocuments || []) validate(document);
      if (provider) provider.refresh();
    }),
    vscode.commands.registerCommand('angels95Ozd.reveal', openAt),
    vscode.commands.registerCommand('angels95Ozd.openWorkspaceDef', openAt)
  );

  for (const document of vscode.workspace.textDocuments || []) validate(document);

  const watcher = vscode.workspace.createFileSystemWatcher('**/*.ozls');
  const pawnWatcher = vscode.workspace.createFileSystemWatcher('**/PawnDefs/*.cfg');
  context.subscriptions.push(
    watcher,
    pawnWatcher,
    watcher.onDidCreate(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    }),
    watcher.onDidDelete(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    }),
    watcher.onDidChange(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    }),
    pawnWatcher.onDidCreate(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    }),
    pawnWatcher.onDidDelete(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    }),
    pawnWatcher.onDidChange(() => {
      provider.invalidateWorkspaceIndex();
      provider.refresh();
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((document) => {
      validate(document);
      if (isTarget(document)) provider.refresh();
    }),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (isTarget(e.document)) {
        invalidate(e.document);
        schedule(e.document);
      }
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      clearTimer(document.uri);
      diagnostics.delete(document.uri);
      if (isTarget(document)) provider.refresh();
    }),
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (isTarget(document)) {
        provider.invalidateWorkspaceIndex();
        validate(document);
        provider.refresh();
      }
    }),
    vscode.window.onDidChangeActiveTextEditor(() => {
      if (provider) provider.refresh();
    })
  );
}

function deactivate() {
  for (const timer of timers.values()) clearTimeout(timer);
  timers.clear();
  if (diagnostics) diagnostics.dispose();
}

module.exports = {
  activate,
  deactivate,
  _test: { validate, buildSymbolsFor: (document) => buildSymbols(getModel(document), document), getModel }
};
