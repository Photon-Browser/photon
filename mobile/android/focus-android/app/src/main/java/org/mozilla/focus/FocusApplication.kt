/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.focus

import android.content.Context
import android.os.Build
import android.os.StrictMode
import android.util.Log.INFO
import androidx.annotation.OpenForTesting
import androidx.annotation.VisibleForTesting
import androidx.appcompat.app.AppCompatDelegate
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.preference.PreferenceManager
import androidx.work.Configuration.Builder
import androidx.work.Configuration.Provider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import mozilla.components.support.AppServicesInitializer
import mozilla.components.support.base.facts.register
import mozilla.components.support.base.log.Log
import mozilla.components.support.base.log.sink.AndroidLogSink
import mozilla.components.support.ktx.android.content.isMainProcess
import mozilla.components.support.locale.LocaleAwareApplication
import mozilla.components.support.rusthttp.RustHttpConfig
import mozilla.components.support.webextensions.WebExtensionSupport
import org.mozilla.focus.biometrics.LockObserver
import org.mozilla.focus.experiments.finishNimbusInitialization
import org.mozilla.focus.ext.settings
import org.mozilla.focus.navigation.StoreLink
import org.mozilla.focus.nimbus.FocusNimbus
import org.mozilla.focus.session.VisibilityLifeCycleCallback
import org.mozilla.focus.telemetry.FactsProcessor
import org.mozilla.focus.telemetry.ProfilerMarkerFactProcessor
import org.mozilla.focus.utils.AppConstants
import kotlin.coroutines.CoroutineContext

@Suppress("TooManyFunctions")
open class FocusApplication : LocaleAwareApplication(), Provider, CoroutineScope {
    private var job = Job()
    override val coroutineContext: CoroutineContext
        get() = job + Dispatchers.Main

    open val components: Components by lazy { Components(this) }

    var visibilityLifeCycleCallback: VisibilityLifeCycleCallback? = null
        private set

    private val storeLink by lazy { StoreLink(components.appStore, components.store) }
    private val lockObserver by lazy { LockObserver(this, components.store, components.appStore) }

    @OptIn(DelicateCoroutinesApi::class)
    override fun onCreate() {
        super.onCreate()

        Log.addSink(AndroidLogSink("Focus"))
        components.crashReporter.install(this)

        if (isMainProcess()) {
            initializeNimbus()

            PreferenceManager.setDefaultValues(this, R.xml.settings, false)

            setTheme(this)
            components.engine.warmUp()

            initializeTelemetry()

            finishSetupMegazord()

            ProfilerMarkerFactProcessor.create { components.engine.profiler }.register()

            enableStrictMode()

            visibilityLifeCycleCallback = VisibilityLifeCycleCallback(this@FocusApplication)
            registerActivityLifecycleCallbacks(visibilityLifeCycleCallback)
            registerComponentCallbacks(visibilityLifeCycleCallback)

            storeLink.start()

            initializeWebExtensionSupport()

            setupLeakCanary()

            components.appStartReasonProvider.registerInAppOnCreate(this)
            components.startupActivityLog.registerInAppOnCreate(this)

            ProcessLifecycleOwner.get().lifecycle.addObserver(lockObserver)
            GlobalScope.launch(Dispatchers.IO) {
                // Remove stale temporary uploaded files.
                components.fileUploadsDirCleaner.cleanUploadsDirectory()
            }
        }
    }

    override fun onConfigurationChanged(config: android.content.res.Configuration) {
        applicationContext.resources.configuration.uiMode = config.uiMode
        super.onConfigurationChanged(config)
    }

    protected open fun setupLeakCanary() {
        // no-op, LeakCanary is disabled by default
    }

    open fun updateLeakCanaryState(isEnabled: Boolean) {
        // no-op, LeakCanary is disabled by default
    }

    protected open fun initializeNimbus() {
        beginSetupMegazord()

        // This lazily constructs the Nimbus object…
        val nimbus = components.experiments
        // … which we then can populate the feature configuration.
        FocusNimbus.initialize { nimbus }
    }

    protected open fun initializeTelemetry() {
        components.metrics.initialize(this)
        FactsProcessor.initialize()
        if (components.settings.isDailyUsagePingEnabled) {
            components.usageReportingMetricsService.start()
        }
    }

    /**
     * Initiate Megazord sequence! Megazord Battle Mode!
     *
     * The application-services combined libraries are known as the "megazord". We use the default `full`
     * megazord - it contains everything that fenix needs, and (currently) nothing more.
     *
     * Documentation on what megazords are, and why they're needed:
     * - https://github.com/mozilla/application-services/blob/master/docs/design/megazords.md
     * - https://mozilla.github.io/application-services/docs/applications/consuming-megazord-libraries.html
     *
     * This is the initialization of the megazord without setting up networking, i.e. needing the
     * engine for networking. This should do the minimum work necessary as it is done on the main
     * thread, early in the app startup sequence.
     */
    private fun beginSetupMegazord() {
        AppServicesInitializer.init(components.crashReporter)
    }

    /**
     * Finish Megazord setup sequence.
     */
    @OptIn(DelicateCoroutinesApi::class) // GlobalScope usage
    @OpenForTesting
    open fun finishSetupMegazord() {
        GlobalScope.launch(Dispatchers.IO) {
            // We need to use an unwrapped client because native components do not support private
            // requests.
            @Suppress("Deprecation")
            RustHttpConfig.setClient(lazy { components.client.unwrap() })

            // Now viaduct (the RustHttp client) is initialized we can ask Nimbus to fetch
            // experiments recipes from the server.
            finishNimbusInitialization(components.experiments)
        }
    }

    private fun setTheme(context: Context) {
        val settings = context.settings
        when {
            settings.lightThemeSelected -> {
                AppCompatDelegate.setDefaultNightMode(
                    AppCompatDelegate.MODE_NIGHT_NO,
                )
            }

            settings.darkThemeSelected -> {
                AppCompatDelegate.setDefaultNightMode(
                    AppCompatDelegate.MODE_NIGHT_YES,
                )
            }

            settings.useDefaultThemeSelected -> {
                setDefaultTheme()
            }

            // No theme setting selected, select the default value, follow device theme.
            else -> {
                setDefaultTheme()
                settings.useDefaultThemeSelected = true
            }
        }
    }

    private fun setDefaultTheme() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            AppCompatDelegate.setDefaultNightMode(
                AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM,
            )
        } else {
            AppCompatDelegate.setDefaultNightMode(
                AppCompatDelegate.MODE_NIGHT_AUTO_BATTERY,
            )
        }
    }

    private fun enableStrictMode() {
        // Only enable StrictMode in debug builds
        if (AppConstants.isDevBuild) {
            val threadPolicyBuilder = StrictMode.ThreadPolicy.Builder().detectAll()
            val vmPolicyBuilder = StrictMode.VmPolicy.Builder()
                .detectActivityLeaks()
                .detectFileUriExposure()
                .detectLeakedClosableObjects()
                .detectLeakedRegistrationObjects()
                .detectLeakedSqlLiteObjects()

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                vmPolicyBuilder.detectNonSdkApiUsage()
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                vmPolicyBuilder.detectUnsafeIntentLaunch()
            }

            threadPolicyBuilder.penaltyLog()
            vmPolicyBuilder.penaltyLog()

            StrictMode.setThreadPolicy(threadPolicyBuilder.build())
            StrictMode.setVmPolicy(vmPolicyBuilder.build())
        }
    }

    @VisibleForTesting
    @OpenForTesting
    internal open fun initializeWebExtensionSupport() {
        WebExtensionSupport.initialize(
            components.engine,
            components.store,
            onNewTabOverride = { _, engineSession, url ->
                components.tabsUseCases.addTab(
                    url = url,
                    selectTab = true,
                    engineSession = engineSession,
                    private = true,
                )
            },
        )
    }

    override val workManagerConfiguration = Builder().setMinimumLoggingLevel(INFO).build()
}
