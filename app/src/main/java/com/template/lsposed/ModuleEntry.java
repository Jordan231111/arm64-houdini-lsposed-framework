package com.template.lsposed;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.template.lsposed.engine.EngineDetector;
import com.template.lsposed.ui.OverlayController;

import java.lang.reflect.Method;
import java.util.Locale;

import io.github.libxposed.api.XposedInterface;
import io.github.libxposed.api.XposedModule;

/**
 * Modern libxposed API 101 entry point.
 *
 * <p>Registered in {@code META-INF/xposed/java_init.list}. The framework calls this class in each
 * scoped target process. Process-level filtering is enforced here (see {@link TemplateConfig}) so
 * the module does not bloat satellite processes such as {@code :push}, {@code :pushservice},
 * {@code :gameservice} or dedicated anti-cheat processes that many games ship.</p>
 */
public final class ModuleEntry extends XposedModule {
    private volatile String packageName;
    private volatile String processName;
    private volatile boolean applicationAttachHookInstalled;
    private volatile boolean applicationContextReady;
    private volatile boolean activityHookInstalled;
    private volatile boolean nativeHookToggleListenerInstalled;
    private volatile boolean nativeHooksStarted;

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        try {
            processName = param.getProcessName();
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.INFO, TemplateConfig.LOG_TAG, "Loaded in process=" + processName
                        + ", framework=" + getFrameworkName()
                        + " " + getFrameworkVersion()
                        + ", api=" + getApiVersion());
            }
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) Log.e(TemplateConfig.LOG_TAG, "onModuleLoaded log failed", t);
        }
    }

    @Override
    public void onPackageLoaded(PackageLoadedParam param) {
        preparePackage(param, "onPackageLoaded");
    }

    @Override
    public void onPackageReady(PackageReadyParam param) {
        preparePackage(param, "onPackageReady");
    }

    private void preparePackage(PackageLoadedParam param, String phase) {
        String pkg = param.getPackageName();
        String proc = processName != null ? processName : pkg;
        if (!TemplateConfig.shouldHookProcess(pkg, proc)) return;
        packageName = pkg;

        if (TemplateConfig.VERBOSE_LOGS) {
            log(Log.INFO, TemplateConfig.LOG_TAG, phase + " " + packageName
                    + " process=" + proc
                    + " firstPackage=" + param.isFirstPackage()
                    + " dataDir=" + param.getApplicationInfo().dataDir);
        }

        installApplicationAttachHook();
        // Install always; the hook itself checks the registry so users can flip it live.
        installActivityResumeHook();
    }

    private synchronized void installApplicationAttachHook() {
        if (applicationAttachHookInstalled) return;
        applicationAttachHookInstalled = true;
        try {
            Method attach = Application.class.getDeclaredMethod("attach", Context.class);
            attach.setAccessible(true);
            hook(attach)
                    .setPriority(XposedInterface.PRIORITY_HIGHEST)
                    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                    .intercept(chain -> {
                        Object result = chain.proceed();
                        try {
                            Object thiz = chain.getThisObject();
                            Context ctx = thiz instanceof Context ? (Context) thiz : null;
                            if (ctx == null && !chain.getArgs().isEmpty() && chain.getArg(0) instanceof Context) {
                                ctx = (Context) chain.getArg(0);
                            }
                            if (ctx != null) onApplicationContextReady(ctx);
                        } catch (Throwable t) {
                            if (TemplateConfig.VERBOSE_LOGS) {
                                log(Log.ERROR, TemplateConfig.LOG_TAG, "Application.attach post-hook failed", t);
                            }
                        }
                        return result;
                    });
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.INFO, TemplateConfig.LOG_TAG, "Hooked Application.attach");
            }
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.ERROR, TemplateConfig.LOG_TAG, "Could not hook Application.attach", t);
            }
        }
    }

    private synchronized void onApplicationContextReady(Context context) {
        if (applicationContextReady) return;
        applicationContextReady = true;
        Context appContext = context.getApplicationContext() != null ? context.getApplicationContext() : context;

        FeatureRegistry.initialize(appContext);

        try {
            EngineDetector.Engine engine = EngineDetector.detect(appContext);
            FeatureState.setEngineLabel(engine.name().toLowerCase(Locale.US));
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.INFO, TemplateConfig.LOG_TAG, "Detected engine=" + engine
                        + " evidence=" + EngineDetector.evidenceFromNativeLibraryDir(appContext));
            }
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.WARN, TemplateConfig.LOG_TAG, "Engine detection failed", t);
            }
        }

        installNativeHookToggleListener(appContext);
        maybeStartNativeHooks(appContext);

        if (TemplateConfig.ENABLE_OVERLAY) {
            new Handler(Looper.getMainLooper()).post(() -> OverlayController.attach(appContext));
        }
    }

    private synchronized void installNativeHookToggleListener(Context appContext) {
        if (nativeHookToggleListenerInstalled) return;
        nativeHookToggleListenerInstalled = true;
        FeatureRegistry.addListener(key -> {
            if (FeatureRegistry.KEY_ENABLED.equals(key) || FeatureRegistry.KEY_NATIVE_HOOKS.equals(key)) {
                maybeStartNativeHooks(appContext);
            }
        });
    }

    private synchronized void maybeStartNativeHooks(Context appContext) {
        if (nativeHooksStarted) return;
        if (!FeatureRegistry.getBool(FeatureRegistry.KEY_ENABLED)) return;
        if (!FeatureRegistry.getBool(FeatureRegistry.KEY_NATIVE_HOOKS)) return;
        nativeHooksStarted = true;
        Thread worker = new Thread(
                () -> NativeBridge.installNativeHooks(appContext, packageName),
                TemplateConfig.WORKER_THREAD_NAME);
        worker.setDaemon(true);
        worker.start();
    }

    private synchronized void installActivityResumeHook() {
        if (activityHookInstalled) return;
        activityHookInstalled = true;
        try {
            Method onResume = Activity.class.getDeclaredMethod("onResume");
            onResume.setAccessible(true);
            hook(onResume)
                    .setPriority(XposedInterface.PRIORITY_LOWEST)
                    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                    .intercept(chain -> {
                        Object result = chain.proceed();
                        try {
                            if (!FeatureRegistry.getBool(FeatureRegistry.KEY_ENABLED)) return result;
                            if (!FeatureRegistry.getBool(FeatureRegistry.KEY_SAMPLE_ACTIVITY_HOOK)) return result;
                            int hits = FeatureState.bumpJavaHookHits();
                            Object thiz = chain.getThisObject();
                            String activityName = thiz != null ? thiz.getClass().getName() : "<null>";
                            FeatureState.setLastMessage("Activity.onResume #" + hits + ": " + activityName);
                            if (TemplateConfig.VERBOSE_LOGS) {
                                log(Log.INFO, TemplateConfig.LOG_TAG,
                                        "Activity.onResume #" + hits + " -> " + activityName);
                            }
                        } catch (Throwable t) {
                            if (TemplateConfig.VERBOSE_LOGS) {
                                log(Log.ERROR, TemplateConfig.LOG_TAG,
                                        "Activity.onResume post-hook failed", t);
                            }
                        }
                        return result;
                    });
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                deoptimize(onResume);
            }
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.INFO, TemplateConfig.LOG_TAG, "Hooked Activity.onResume sample");
            }
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) {
                log(Log.ERROR, TemplateConfig.LOG_TAG, "Could not hook Activity.onResume", t);
            }
        }
    }
}
