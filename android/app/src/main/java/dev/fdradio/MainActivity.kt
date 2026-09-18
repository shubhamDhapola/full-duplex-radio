package dev.fdradio

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    MainScreen()
                }
            }
        }
    }

    override fun onStop() {
        super.onStop()
        // The audio device is a shared resource and holding it in the
        // background is antisocial -- and on some devices the platform will
        // take it away mid-callback, which is a harder failure to read than
        // simply having released it.
        AudioEngine.stop()
    }
}

@Composable
private fun MainScreen() {
    val context = LocalContext.current

    var granted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    val requestPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted = it }

    var running by remember { mutableStateOf(false) }
    var capture by remember { mutableStateOf(AudioEngine.Capture.VOICE_COMMUNICATION) }
    var snapshot by remember { mutableStateOf<AudioEngine.Snapshot?>(null) }

    // Polling, at 5 Hz. The audio callback never calls up into Kotlin -- see
    // AudioEngine.kt for why the direction is inverted.
    LaunchedEffect(running) {
        while (true) {
            snapshot = AudioEngine.snapshot()
            delay(200)
        }
    }

    val report = remember {
        if (NativeCore.available) {
            runCatching { NativeCore.selfCheck() }.getOrElse { "native call failed: ${it.message}" }
        } else {
            "libfdradio_jni did not load"
        }
    }
    val abi = remember {
        if (NativeCore.available) runCatching { NativeCore.abi() }.getOrDefault("?")
        else Build.SUPPORTED_ABIS.firstOrNull() ?: "?"
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Text("Full-Duplex Radio", style = MaterialTheme.typography.headlineSmall)
        Text(
            "$abi · Android ${Build.VERSION.SDK_INT} · ${Build.MODEL}",
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.padding(top = 4.dp, bottom = 16.dp),
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Button(
                onClick = {
                    if (!granted) {
                        requestPermission.launch(Manifest.permission.RECORD_AUDIO)
                    } else if (running) {
                        AudioEngine.stop()
                        running = false
                    } else {
                        running = AudioEngine.start(capture)
                    }
                }
            ) {
                Text(
                    when {
                        !granted -> "Grant microphone"
                        running -> "Stop loopback"
                        else -> "Start loopback"
                    }
                )
            }
        }

        Text(
            "capture chain",
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(top = 16.dp, bottom = 4.dp),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            AudioEngine.Capture.entries.forEach { option ->
                FilterChip(
                    selected = capture == option,
                    // Changing the chain means reopening the streams, so it is
                    // only offered while stopped. Silently restarting under the
                    // user would also reset every counter on the screen.
                    enabled = !running,
                    onClick = { capture = option },
                    label = { Text(option.label, style = MaterialTheme.typography.labelSmall) },
                )
            }
        }

        Text(
            "Loopback wires the microphone straight to the speaker. Use a wired " +
                "headset — on the speaker it will feed back.",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(top = 12.dp, bottom = 16.dp),
        )

        snapshot?.let { AudioCard(it) }

        Text(
            "core self-check",
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(top = 20.dp, bottom = 6.dp),
        )
        Text(report, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun AudioCard(s: AudioEngine.Snapshot) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(14.dp)) {
            Text(
                if (s.running) "audio running" else "audio stopped",
                style = MaterialTheme.typography.titleSmall,
            )
            val lines = buildString {
                fun row(label: String, value: String) =
                    append(label.padEnd(16)).append(value).append('\n')

                row("capture", s.capture.label)
                if (s.running) {
                    row("api", "in ${api(s.inputAAudio)} / out ${api(s.outputAAudio)}")
                    row(
                        "performance",
                        "in ${mode(s.inputLowLatency)} / out ${mode(s.outputLowLatency)}",
                    )
                } else {
                    // With no stream open there is nothing to report, and
                    // printing the defaults would read as a finding.
                    row("api", "-")
                    row("performance", "-")
                }
                row("rate", "${s.inputSampleRate} / ${s.outputSampleRate} Hz")
                row(
                    "burst",
                    "${s.inputBurstFrames} / ${s.outputBurstFrames} frames " +
                        "(${fmt(s.outputBurstMillis)} ms)",
                )
                row(
                    "buffer",
                    "${s.inputBufferFrames}/${s.inputCapacityFrames} · " +
                        "${s.outputBufferFrames}/${s.outputCapacityFrames}",
                )
                row("out latency", "${fmt(s.outputLatencyUs / 1000.0)} ms")
                row("ring", "${s.ringSamples} samples (${fmt(s.ringMillis)} ms)")
                row("callbacks", "in ${s.inputCallbacks} / out ${s.outputCallbacks}")
                row("worst gap", "${fmt(s.worstOutputGapUs / 1000.0)} ms")
                row("xruns", "in ${s.inputXRuns} / out ${s.outputXRuns}")
                row("ring over/under", "${s.ringOverflows} / ${s.ringUnderruns}")
                if (s.primedSamples > 0) row("primed away", "${s.primedSamples} samples")
                if (s.lastError != 0) row("error", AudioEngine.errorText(s.lastError))
            }
            Text(
                lines.trimEnd(),
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(top = 8.dp),
            )
        }
    }
}

private fun api(aaudio: Boolean) = if (aaudio) "AAudio" else "OpenSL"

private fun mode(lowLatency: Boolean) = if (lowLatency) "LowLatency" else "NOT-LOW"

private fun fmt(value: Double) = String.format("%.2f", value)
