'use strict';

Process.enumerateModules()
  .sort(function (a, b) { return a.name.localeCompare(b.name); })
  .forEach(function (m) {
    console.log(m.name + ' base=' + m.base + ' size=' + m.size + ' path=' + m.path);
  });
