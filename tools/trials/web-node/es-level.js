#!/usr/bin/env node
// Spike S5: the lowest ECMAScript edition in which a JavaScript file parses, found with the acorn
// parser that ships with the emsdk (<emscripten>/node_modules/acorn). ES5 is what Internet
// Explorer 9, Firefox 8, Chrome 15, Safari 5.1 and Opera 11.5 can parse; a file that needs a
// later edition (arrow functions, async functions, optional chaining, ...) cannot even load there.
//
//   node es-level.js <acorn directory> <file.js>
"use strict";
var acorn = require(require("path").resolve(process.argv[2]));
var source = require("fs").readFileSync(process.argv[3], "utf8");
var editions = [3, 5, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022, 2023, 2024, 2025, "latest"];
console.log("acorn " + acorn.version + ", file " + process.argv[3] + " (" + source.length + " bytes)");
var firstError = "";
for (var i = 0; i < editions.length; i++) {
  try {
    acorn.parse(source, { ecmaVersion: editions[i] });
    console.log("lowest edition that parses: " + (editions[i] === 5 ? "ES5" : "ecmaVersion " + editions[i]));
    if (firstError) console.log("ES5 error: " + firstError);
    process.exit(editions[i] === 3 || editions[i] === 5 ? 0 : 1);
  } catch (e) {
    if (!firstError) firstError = e.message + " near: " + JSON.stringify(source.substr(e.pos - 20, 60));
  }
}
console.log("does not parse in any edition: " + firstError);
process.exit(2);
