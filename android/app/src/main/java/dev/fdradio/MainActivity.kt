package dev.fdradio

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.input.pointer.pointerInput
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
                Surface(modifier = Modifier.fillMaxSize()) { MainScreen() }
            }
        }
    }

    override fun onStop() {
        super.onStop()
        // The audio device is shared; holding it in the background is
        // antisocial, and on some devices the platform takes it away
        // mid-callback, which is a harder failure to read than having released
        // it deliberately.
        AudioEngine.stop()
        Session.stop()
    }
}

private enum class Mode { LOOPBACK, SESSION }

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

    var mode by remember { mutableStateOf(Mode.SESSION) }
    var capture by remember { mutableStateOf(AudioEngine.Capture.VOICE_RECOGNITION) }
    var peer by remember { mutableStateOf("127.0.0.1") }
    var port by remember { mutableStateOf("47000") }
    var localPort by remember { mutableStateOf("47000") }
    var fec by remember { mutableStateOf(false) }
    var running by remember { mutableStateOf(false) }
    var failure by remember { mutableStateOf<String?>(null) }

    var audio by remember { mutableStateOf<AudioEngine.Snapshot?>(null) }
    var session by remember { mutableStateOf<Session.Snapshot?>(null) }

    LaunchedEffect(Unit) {
        while (true) {
            audio = AudioEngine.snapshot()
            session = Session.snapshot()
            delay(200)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Text("Full-Duplex Radio", style = MaterialTheme.typography.headlineSmall)
        Text(
            "${Build.MODEL} · Android ${Build.VERSION.SDK_INT}",
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.padding(top = 4.dp, bottom = 14.dp),
        )

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(
                selected = mode == Mode.SESSION,
                enabled = !running,
                onClick = { mode = Mode.SESSION },
                label = { Text("transport") },
            )
            FilterChip(
                selected = mode == Mode.LOOPBACK,
                enabled = !running,
                onClick = { mode = Mode.LOOPBACK },
                label = { Text("loopback") },
            )
            FilterChip(
                selected = fec,
                enabled = !running && mode == Mode.SESSION,
                onClick = { fec = !fec },
                label = { Text("FEC") },
            )
        }

        if (mode == Mode.SESSION) {
            Row(
                modifier = Modifier.padding(top = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedTextField(
                    value = peer,
                    onValueChange = { peer = it },
                    enabled = !running,
                    label = { Text("peer") },
                    singleLine = true,
                    modifier = Modifier.weight(2f),
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = { port = it },
                    enabled = !running,
                    label = { Text("port") },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = localPort,
                    onValueChange = { localPort = it },
                    enabled = !running,
                    label = { Text("local") },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
            }
        }

        Row(
            modifier = Modifier.padding(top = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            AudioEngine.Capture.entries.forEach { option ->
                FilterChip(
                    selected = capture == option,
                    enabled = !running,
                    onClick = { capture = option },
                    label = { Text(option.label, style = MaterialTheme.typography.labelSmall) },
                )
            }
        }

        Button(
            modifier = Modifier.padding(top = 14.dp),
            onClick = {
                failure = null
                if (!granted) {
                    requestPermission.launch(Manifest.permission.RECORD_AUDIO)
                    return@Button
                }
                if (running) {
                    AudioEngine.stop()
                    Session.stop()
                    running = false
                    return@Button
                }
                if (mode == Mode.SESSION) {
                    val ok = Session.start(
                        peerHost = peer.trim(),
                        peerPort = port.trim().toIntOrNull() ?: 47000,
                        localPort = localPort.trim().toIntOrNull() ?: 47000,
                        fec = fec,
                        expectedLoss = if (fec) 20 else 0,
                    )
                    if (!ok) {
                        failure = "session failed to start (address, or port in use)"
                        return@Button
                    }
                }
                running = AudioEngine.start(capture, withSession = mode == Mode.SESSION)
                if (!running) {
                    Session.stop()
                    failure = "audio failed to start"
                }
            },
        ) {
            Text(
                when {
                    !granted -> "Grant microphone"
                    running -> "Stop"
                    mode == Mode.SESSION -> "Connect"
                    else -> "Start loopback"
                }
            )
        }

        failure?.let {
            Text(
                it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(top = 8.dp),
            )
        }

        if (running && mode == Mode.SESSION) {
            PushToTalk(modifier = Modifier.padding(top = 14.dp))
        }

        Text(
            if (mode == Mode.SESSION) {
                "Audio is sent only while the button is held. Use a wired headset."
            } else {
                "Loopback wires the microphone to the speaker. Use a wired headset."
            },
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(top = 10.dp, bottom = 14.dp),
        )

        session?.takeIf { it.running }?.let { SessionCard(it) }
        audio?.let { AudioCard(it) }

        // The core self-check, run once at start-up on an otherwise idle
        // thread. Its encode and decode figures are the uncontended baseline
        // the session's in-flight figures are compared against.
        val report = remember {
            if (NativeCore.available) {
                runCatching { NativeCore.selfCheck() }
                    .getOrElse { "native call failed: ${it.message}" }
            } else {
                "libfdradio_jni did not load"
            }
        }
        Text(
            "core self-check",
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(top = 18.dp, bottom = 6.dp),
        )
        Text(
            report,
            fontFamily = FontFamily.Monospace,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun PushToTalk(modifier: Modifier = Modifier) {
    var held by remember { mutableStateOf(false) }

    Card(
        modifier = modifier
            .fillMaxWidth()
            .height(96.dp)
            // A press-and-hold gesture rather than a toggle, because that is
            // what push to talk means and because releasing the finger is the
            // one action that must never be missed: a stuck-open microphone is
            // the worst failure this app has.
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        held = true
                        Session.setTransmitting(true)
                        try {
                            awaitRelease()
                        } finally {
                            held = false
                            Session.setTransmitting(false)
                        }
                    }
                )
            },
        colors = CardDefaults.cardColors(
            containerColor = if (held) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.surfaceVariant
            }
        ),
    ) {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(
                if (held) "TRANSMITTING" else "HOLD TO TALK",
                style = MaterialTheme.typography.titleMedium,
                color = if (held) {
                    MaterialTheme.colorScheme.onPrimary
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
    }
}

@Composable
private fun SessionCard(s: Session.Snapshot) {
    Card(modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp)) {
        Column(modifier = Modifier.padding(14.dp)) {
            Text(
                if (s.transmitting) "transport · transmitting" else "transport",
                style = MaterialTheme.typography.titleSmall,
            )
            val lines = buildString {
                fun row(label: String, value: String) =
                    append(label.padEnd(15)).append(value).append('\n')

                row("sent", "${s.packetsSent} pkt, ${s.bytesSent / 1024} KiB, ${s.talkspurts} talkspurt")
                row("received", "${s.datagramsReceived} pkt, ${s.rejected} rejected")
                row("played", "${s.fromPacket} pkt / ${s.fecRecovered} fec / ${s.concealed} concealed")
                // `late` only. The queue's `gaps` counter also ticks for
                // every idle position after a talkspurt ends -- 105 of them in
                // a five-second test -- so on this screen it reads as packet
                // loss when nothing was lost. What was actually missing while
                // audio was flowing is the fec and concealed figures above.
                row("late", "${s.late}")
                row("duplicates", "${s.duplicates}")
                row("buffer depth", "${s.depthFrames} frames (${fmt(s.depthMillis)} ms)")
                row("encode", "${fmt(s.encodeMeanUs)} us mean, ${s.encodeMaxUs} max")
                row("decode", "${fmt(s.decodeMeanUs)} us mean, ${s.decodeMaxUs} max")
                if (s.sendFailed > 0) row("send failed", "${s.sendFailed}")
                if (s.encodeFailed > 0) row("encode failed", "${s.encodeFailed}")
                if (s.rxRingOverflows > 0) row("rx overflow", "${s.rxRingOverflows}")
                if (s.txPcmOverflows > 0) row("tx overflow", "${s.txPcmOverflows}")
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
                    append(label.padEnd(15)).append(value).append('\n')

                row("capture", s.capture.label)
                if (s.running) {
                    row("api", "in ${api(s.inputAAudio)} / out ${api(s.outputAAudio)}")
                    row("performance", "in ${mode(s.inputLowLatency)} / out ${mode(s.outputLowLatency)}")
                    row("burst", "${s.inputBurstFrames} / ${s.outputBurstFrames} frames")
                    row("out latency", "${fmt(s.outputLatencyUs / 1000.0)} ms")
                    row("worst gap", "${fmt(s.worstOutputGapUs / 1000.0)} ms")
                    row("xruns", "in ${s.inputXRuns} / out ${s.outputXRuns}")
                } else {
                    row("api", "-")
                }
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
