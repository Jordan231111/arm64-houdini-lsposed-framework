'use strict';

function log(message) {
  console.log('[recon] ' + message);
}

log('pid=' + Process.id + ' arch=' + Process.arch + ' platform=' + Process.platform);

function hookNativeLoaders() {
  const names = ['android_dlopen_ext', 'dlopen'];
  names.forEach(function (name) {
    const addr = Module.findExportByName(null, name);
    if (!addr) {
      log('native loader not found: ' + name);
      return;
    }
    Interceptor.attach(addr, {
      onEnter(args) {
        this.path = args[0].isNull() ? '<null>' : args[0].readCString();
      },
      onLeave(retval) {
        log(name + '(' + this.path + ') -> ' + retval);
      }
    });
    log('hooked native loader ' + name + ' at ' + addr);
  });
}

function listInterestingModules() {
  const modules = Process.enumerateModules();
  log('module count=' + modules.length);
  modules.forEach(function (m) {
    const n = m.name.toLowerCase();
    if (n.indexOf('il2cpp') >= 0 || n.indexOf('unity') >= 0 || n.indexOf('art') >= 0 || n.indexOf('libc.so') >= 0) {
      log('module ' + m.name + ' base=' + m.base + ' size=' + m.size + ' path=' + m.path);
    }
  });
}

function javaRecon() {
  if (!Java.available) {
    log('Java runtime unavailable');
    return;
  }

  Java.perform(function () {
    try {
      const ActivityThread = Java.use('android.app.ActivityThread');
      const app = ActivityThread.currentApplication();
      if (app) {
        const ctx = app.getApplicationContext();
        log('package=' + ctx.getPackageName());
        log('dataDir=' + ctx.getApplicationInfo().dataDir.value);
        log('nativeLibraryDir=' + ctx.getApplicationInfo().nativeLibraryDir.value);
      }
    } catch (e) {
      log('ActivityThread info failed: ' + e);
    }

    try {
      const Activity = Java.use('android.app.Activity');
      Activity.onResume.implementation = function () {
        log('Activity.onResume ' + this.getClass().getName());
        return this.onResume();
      };
      log('hooked Activity.onResume');
    } catch (e) {
      log('Activity.onResume hook failed: ' + e);
    }

    try {
      const Runtime = Java.use('java.lang.Runtime');
      Runtime.loadLibrary0.overloads.forEach(function (overload) {
        overload.implementation = function () {
          const args = Array.prototype.slice.call(arguments);
          const name = args.length > 0 ? args[args.length - 1] : '<unknown>';
          log('Runtime.loadLibrary0 ' + name);
          return overload.apply(this, arguments);
        };
      });
      log('hooked Runtime.loadLibrary0 overloads=' + Runtime.loadLibrary0.overloads.length);
    } catch (e) {
      log('Runtime.loadLibrary0 hook failed: ' + e);
    }

    try {
      const System = Java.use('java.lang.System');
      System.load.overload('java.lang.String').implementation = function (path) {
        log('System.load ' + path);
        return this.load(path);
      };
      log('hooked System.load');
    } catch (e) {
      log('System.load hook failed: ' + e);
    }
  });
}

hookNativeLoaders();
javaRecon();
setTimeout(listInterestingModules, 1500);
setTimeout(listInterestingModules, 5000);
