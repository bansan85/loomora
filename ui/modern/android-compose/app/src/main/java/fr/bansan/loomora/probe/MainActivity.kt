package fr.bansan.loomora.probe

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Modern UI of Android (API 24 and later, Kotlin and Jetpack Compose): a text field pre-filled
 * "Coucou" limited to 1,000 characters (code points), a label with the greeting computed by the
 * backend, the end-of-life warning, and the probe mode. No application logic: the greeting, the
 * end-of-life verdict and the probe line all come from Native.
 */
class MainActivity : ComponentActivity() {
    private val handler = Handler(Looper.getMainLooper())
    private var launchMs = 0L
    private var probe = false
    private var displayed = false
    private var displayMs = 0
    private var emitted = false
    private var eolShown = false
    private var shownInput = ""
    private var shownGreeting = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        launchMs = SystemClock.elapsedRealtime()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // A project mode (spec 002) is exclusive with the probe: with both, nothing is written.
        val projectMode = ProjectProbe.mode(intent)
        probe = Probe.enabled(intent) && projectMode == ProjectProbe.Mode.NONE

        val ready = Native.loaded && Native.abiVersion() == EXPECTED_ABI
        val problem: String? = when {
            ready -> null
            Native.loaded ->
                "The backend does not match this UI (ABI version ${Native.abiVersion()}, expected $EXPECTED_ABI)."
            else -> "The backend could not be loaded: ${Native.loadError}"
        }
        var banner: String? = null
        if (ready) {
            val v = Probe.androidVersion()
            if (Native.isEol(FAMILY_ANDROID, v[0], v[1]) == 1) {
                banner = Native.eolWarning()
                eolShown = banner != null
            }
        }
        val override = if (probe) Probe.inputOverride(intent) else null
        val initial = limited(override ?: "Coucou")

        setContent {
            LoomoraTheme {
                LoomoraScreen(
                    problem = problem,
                    banner = banner,
                    initial = initial,
                    onShown = { input, message ->
                        shownInput = input
                        shownGreeting = message
                    },
                    onFirstDraw = { onFirstDraw() },
                )
            }
        }
        if (probe) handler.postDelayed(emitAndFinish, GIVE_UP_MS)
        if (projectMode != ProjectProbe.Mode.NONE) runProjectMode(projectMode)
    }

    /**
     * Project probe mode (intent extra validate_project or roundtrip_project): the work is done by
     * ProjectProbe on a worker thread, which logs the one line (tag LOOMORA_VALIDATE); the activity
     * then finishes by itself. A usage error (two modes, or a mode next to "probe") logs nothing.
     */
    private fun runProjectMode(mode: ProjectProbe.Mode) {
        val path = ProjectProbe.path(intent, mode)
        Thread {
            try {
                if (mode != ProjectProbe.Mode.BAD_USAGE && path != null) {
                    if (Native.loaded) {
                        ProjectProbe.runAndLog(mode, path)
                    } else {
                        Log.e(ProjectProbe.TAG + "_ERROR", "the backend could not be loaded: ${Native.loadError}")
                    }
                }
            } catch (t: Throwable) {
                Log.e(ProjectProbe.TAG + "_ERROR", "project mode failed: $t")
            }
            handler.post { finish() }
        }.start()
    }

    private fun onFirstDraw() {
        if (displayed) return
        displayed = true
        displayMs = (SystemClock.elapsedRealtime() - launchMs).toInt()
        if (probe) handler.postDelayed(emitAndFinish, Probe.EXIT_DELAY_MS)
    }

    private val emitAndFinish = Runnable {
        if (!emitted) {
            emitted = true
            if (Native.loaded) {
                Probe.emit(shownInput, shownGreeting, displayed, displayMs, eolShown, Contrast.disabledFeatures())
            }
            finish()
        }
    }

    override fun onDestroy() {
        handler.removeCallbacks(emitAndFinish)
        super.onDestroy()
    }

    private companion object {
        const val EXPECTED_ABI = 1 // LOOMORA_ABI_VERSION
        const val FAMILY_ANDROID = 4 // LOOMORA_FAMILY_ANDROID
        const val GIVE_UP_MS = 10000L // probe: emitted anyway if nothing was drawn
    }
}

private const val MAX_CHARS = 1000

/** Keeps at most MAX_CHARS code points, never cutting a surrogate pair in two. */
private fun limited(text: String): String =
    if (text.codePointCount(0, text.length) <= MAX_CHARS) text
    else text.substring(0, text.offsetByCodePoints(0, MAX_CHARS))

@Composable
private fun LoomoraScreen(
    problem: String?,
    banner: String?,
    initial: String,
    onShown: (String, String) -> Unit,
    onFirstDraw: () -> Unit,
) {
    var text by rememberSaveable { mutableStateOf(initial) }
    val greeting = if (problem != null) problem else Native.greet(text) ?: "The greeting could not be computed."
    SideEffect { onShown(text, greeting) }
    val drawn = remember { BooleanArray(1) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .safeDrawingPadding()
            .padding(16.dp),
    ) {
        if (banner != null) {
            // Fixed black on pale yellow (about 19:1), whatever the theme.
            Text(
                text = banner,
                color = Color.Black,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFFFFF3CD))
                    .padding(8.dp),
            )
            Spacer(Modifier.height(12.dp))
        }
        OutlinedTextField(
            value = text,
            onValueChange = { text = limited(it) },
            singleLine = true,
            enabled = problem == null,
            modifier = Modifier.fillMaxWidth(),
        )
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = greeting,
                fontSize = 24.sp,
                textAlign = TextAlign.Center,
                color = MaterialTheme.colorScheme.onBackground,
                modifier = Modifier
                    .verticalScroll(rememberScrollState())
                    .drawWithContent {
                        drawContent()
                        if (!drawn[0]) {
                            drawn[0] = true
                            onFirstDraw()
                        }
                    },
            )
        }
    }
}
