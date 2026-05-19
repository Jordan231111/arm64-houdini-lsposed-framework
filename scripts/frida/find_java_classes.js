'use strict';

// Edit this regex for your app. Example: /login|crypto|billing|unity|lua/i
const PATTERN = /example|target/i;

if (!Java.available) {
  console.log('Java runtime unavailable');
} else {
  Java.perform(function () {
    let count = 0;
    Java.enumerateLoadedClasses({
      onMatch(name) {
        if (PATTERN.test(name)) {
          count += 1;
          console.log(name);
        }
      },
      onComplete() {
        console.log('matched classes=' + count);
      }
    });
  });
}
