/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash

import android.content.Intent
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CrashTest {

    @Test
    fun `fromIntent() can deserialize a GeckoView crash Intent`() {
        val originalCrash = Crash.NativeCodeCrash(
            123,
            "/data/data/org.mozilla.samples.browser/files/mozilla/Crash Reports/pending/3ba5f665-8422-dc8e-a88e-fc65c081d304.dmp",
            "/data/data/org.mozilla.samples.browser/files/mozilla/Crash Reports/pending/3ba5f665-8422-dc8e-a88e-fc65c081d304.extra",
            Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD,
            processType = "content",
            breadcrumbs = arrayListOf(),
            remoteType = "web",
        )

        val intent = Intent()
        originalCrash.fillIn(intent)

        val recoveredCrash = Crash.fromIntent(intent) as? Crash.NativeCodeCrash
            ?: throw AssertionError("Expected NativeCodeCrash instance")

        assertEquals(recoveredCrash.timestamp, 123)
        assertEquals(recoveredCrash.isFatal, false)
        assertEquals(recoveredCrash.processVisibility, Crash.NativeCodeCrash.PROCESS_VISIBILITY_FOREGROUND_CHILD)
        assertEquals(recoveredCrash.processType, "content")
        assertEquals(
            "/data/data/org.mozilla.samples.browser/files/mozilla/Crash Reports/pending/3ba5f665-8422-dc8e-a88e-fc65c081d304.dmp",
            recoveredCrash.minidumpPath,
        )
        assertEquals(
            "/data/data/org.mozilla.samples.browser/files/mozilla/Crash Reports/pending/3ba5f665-8422-dc8e-a88e-fc65c081d304.extra",
            recoveredCrash.extrasPath,
        )
        assertEquals("web", recoveredCrash.remoteType)
    }

    @Test
    fun `Serialize and deserialize UncaughtExceptionCrash`() {
        val exception = RuntimeException("Hello World!")
        val runtimeTags = mapOf("one" to "two", "three" to "four")

        val originalCrash = Crash.UncaughtExceptionCrash(
            timestamp = 0,
            throwable = exception,
            breadcrumbs = arrayListOf(),
            runtimeTags = runtimeTags,
        )

        val intent = Intent()
        originalCrash.fillIn(intent)

        val recoveredCrash = Crash.fromIntent(intent) as? Crash.UncaughtExceptionCrash
            ?: throw AssertionError("Expected UncaughtExceptionCrash instance")

        assertEquals(exception, recoveredCrash.throwable)
        assertEquals("Hello World!", recoveredCrash.throwable.message)
        assertArrayEquals(exception.stackTrace, recoveredCrash.throwable.stackTrace)
        assert(recoveredCrash.runtimeTags == runtimeTags)
    }

    @Test
    fun `isCrashIntent()`() {
        assertFalse(Crash.isCrashIntent(Intent()))

        assertFalse(
            Crash.isCrashIntent(
                Intent()
                    .putExtra("crash", "I am a crash!"),
            ),
        )

        assertTrue(
            Crash.isCrashIntent(
                Intent().apply {
                    Crash.UncaughtExceptionCrash(0, RuntimeException(), arrayListOf()).fillIn(this)
                },
            ),
        )

        assertTrue(
            Crash.isCrashIntent(
                Intent().apply {
                    val crash = Crash.NativeCodeCrash(
                        0,
                        "",
                        "",
                        "",
                        processType = null,
                        breadcrumbs = arrayListOf(),
                        remoteType = null,
                    )
                    crash.fillIn(this)
                },
            ),
        )
    }
}
