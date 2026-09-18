package dev.fdradio

import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    SelfCheckScreen()
                }
            }
        }
    }
}

@Composable
private fun SelfCheckScreen() {
    // remember, so rotating the device does not re-run the codec. It is cheap,
    // but a check that silently re-runs is a check whose cost nobody notices
    // until it is on the audio path.
    val report = remember {
        if (NativeCore.available) {
            runCatching { NativeCore.selfCheck() }
                .getOrElse { "native call failed: ${it.message}" }
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
        Text(
            text = "Full-Duplex Radio",
            style = MaterialTheme.typography.headlineSmall,
        )
        Text(
            text = "$abi · Android ${Build.VERSION.SDK_INT} · ${Build.MODEL}",
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.padding(top = 4.dp, bottom = 20.dp),
        )
        Text(
            text = report,
            fontFamily = FontFamily.Monospace,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
