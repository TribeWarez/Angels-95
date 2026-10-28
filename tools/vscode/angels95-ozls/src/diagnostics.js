'use strict';

const vscode = require('vscode');

const SEVERITY = {
  error: vscode.DiagnosticSeverity.Error,
  warning: vscode.DiagnosticSeverity.Warning,
  info: vscode.DiagnosticSeverity.Information
};

function collect(model) {
  if (!model || !model.problems) return [];
  return model.problems.map((p) => {
    const line = Math.max(0, p.line | 0);
    const range = new vscode.Range(line, 0, line, Number.MAX_SAFE_INTEGER);
    const d = new vscode.Diagnostic(range, p.message, SEVERITY[p.severity] || vscode.DiagnosticSeverity.Warning);
    d.source = 'Angels95 LightningScript';
    if (p.severity === 'error') d.code = 'ozls-error';
    else if (p.severity === 'info') d.code = 'ozls-note';
    else d.code = 'ozls-warning';
    return d;
  });
}

module.exports = { collect };
