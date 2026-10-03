/*
 * Unofficial LSFG Native extension for Zalith Launcher 2.
 */
package com.movtery.zalithlauncher.ui.screens.content.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.navigation3.runtime.NavKey
import com.movtery.zalithlauncher.R
import com.movtery.zalithlauncher.framegen.LsfgNativeBridge
import com.movtery.zalithlauncher.setting.AllSettings
import com.movtery.zalithlauncher.setting.unit.floatRange
import com.movtery.zalithlauncher.ui.base.BaseScreen
import com.movtery.zalithlauncher.ui.components.AnimatedColumn
import com.movtery.zalithlauncher.ui.screens.NestedNavKey
import com.movtery.zalithlauncher.ui.screens.NormalNavKey
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.CardPosition
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.IntSliderSettingsCard
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.ListSettingsCard
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.SettingsCardColumn
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.SwitchSettingsCard
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun FrameGenerationSettingsScreen(
    key: NestedNavKey.Settings,
    settingsScreenKey: NavKey?,
    mainScreenKey: NavKey?
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var importing by remember { mutableStateOf(false) }
    var dllStatus by remember {
        mutableStateOf(
            when {
                LsfgNativeBridge.isReady(context) -> "LSFG Native pronto"
                LsfgNativeBridge.hasDll(context) -> "Lossless.dll encontrada; cache precisa ser preparado"
                else -> "Nenhuma Lossless.dll importada"
            }
        )
    }

    val dllPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            importing = true
            dllStatus = "Importando e preparando shaders LSFG..."
            scope.launch {
                val status = withContext(Dispatchers.IO) {
                    LsfgNativeBridge.importDll(context, uri)
                }
                dllStatus = LsfgNativeBridge.explain(status)
                importing = false
            }
        }
    }

    BaseScreen(
        Triple(key, mainScreenKey, false),
        Triple(NormalNavKey.Settings.FrameGeneration, settingsScreenKey, false)
    ) { isVisible ->
        AnimatedColumn(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(state = rememberScrollState())
                .padding(all = 12.dp),
            isVisible = isVisible
        ) { scopeAnimation ->
            AnimatedItem(scopeAnimation) { yOffset ->
                val enabled = AllSettings.frameGenEnabled.state

                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .offset { IntOffset(x = 0, y = yOffset.roundToPx()) }
                ) {
                    SettingsCardColumn(modifier = Modifier.fillMaxWidth()) {
                        SwitchSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Top,
                            unit = AllSettings.frameGenEnabled,
                            title = stringResource(R.string.settings_framegen_enable_title),
                            summary = stringResource(R.string.settings_framegen_enable_summary)
                        )

                        ListSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Middle,
                            unit = AllSettings.frameGenProfile,
                            items = listOf("performance", "balanced", "quality"),
                            title = stringResource(R.string.settings_framegen_profile_title),
                            summary = stringResource(R.string.settings_framegen_profile_summary),
                            getItemText = {
                                when (it) {
                                    "performance" -> stringResource(R.string.settings_framegen_profile_performance)
                                    "quality" -> stringResource(R.string.settings_framegen_profile_quality)
                                    else -> stringResource(R.string.settings_framegen_profile_balanced)
                                }
                            },
                            getItemId = { it },
                            enabled = enabled
                        )

                        IntSliderSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Middle,
                            unit = AllSettings.frameGenMultiplier,
                            title = stringResource(R.string.settings_framegen_multiplier_title),
                            summary = stringResource(R.string.settings_framegen_multiplier_summary),
                            valueRange = AllSettings.frameGenMultiplier.floatRange,
                            steps = 1,
                            suffix = "x",
                            enabled = enabled,
                            fineTuningControl = false
                        )

                        IntSliderSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Middle,
                            unit = AllSettings.frameGenTargetFps,
                            title = stringResource(R.string.settings_framegen_target_fps_title),
                            summary = stringResource(R.string.settings_framegen_target_fps_summary),
                            valueRange = AllSettings.frameGenTargetFps.floatRange,
                            suffix = " FPS",
                            enabled = enabled,
                            fineTuningControl = true
                        )

                        SwitchSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Middle,
                            unit = AllSettings.frameGenLowLatency,
                            title = stringResource(R.string.settings_framegen_low_latency_title),
                            summary = stringResource(R.string.settings_framegen_low_latency_summary),
                            enabled = enabled
                        )

                        SwitchSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Middle,
                            unit = AllSettings.frameGenSceneProtection,
                            title = stringResource(R.string.settings_framegen_scene_protection_title),
                            summary = stringResource(R.string.settings_framegen_scene_protection_summary),
                            enabled = enabled
                        )

                        SwitchSettingsCard(
                            modifier = Modifier.fillMaxWidth(),
                            position = CardPosition.Bottom,
                            unit = AllSettings.frameGenHud,
                            title = stringResource(R.string.settings_framegen_hud_title),
                            summary = stringResource(R.string.settings_framegen_hud_summary)
                        )
                    }

                    Spacer(modifier = Modifier.height(12.dp))

                    Button(
                        modifier = Modifier.fillMaxWidth(),
                        enabled = !importing,
                        onClick = { dllPicker.launch(arrayOf("*/*")) }
                    ) {
                        Text(if (importing) "Preparando LSFG..." else "Importar Lossless.dll")
                    }

                    Spacer(modifier = Modifier.height(8.dp))
                    Text(dllStatus)
                    Spacer(modifier = Modifier.height(4.dp))
                    Text("A DLL é usada somente como fonte dos shaders LSFG e não é executada no Android.")
                }
            }
        }
    }
}
