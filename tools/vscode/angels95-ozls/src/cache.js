'use strict';

const P = require('./parser');

const cache = new WeakMap();

function getModel(document) {
  const hit = cache.get(document);
  if (hit && hit.version === document.version) return hit.model;
  const model = P.parseAny(document.getText(), document.languageId);
  cache.set(document, { version: document.version, model });
  return model;
}

function invalidate(document) {
  cache.delete(document);
}

module.exports = { getModel, invalidate };
