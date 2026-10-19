package me.magnum.melonds.ui.emulator.input.componentbuilder

import android.content.Context
import android.view.View
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.ui.common.LayoutComponentViewBuilder
import me.magnum.melonds.ui.emulator.input.view.ToggleableImageView

class ToggleableSingleButtonLayoutComponentViewBuilder(private val layoutComponent: LayoutComponent) : LayoutComponentViewBuilder() {

    override fun build(context: Context): View {
        return ToggleableImageView(context).apply {
            val drawables = getToggleableImageViewDrawables()
            enabledDrawable = drawables.first
            disabledDrawable = drawables.second
        }
    }

    override fun getAspectRatio() = 1f

    private fun getToggleableImageViewDrawables(): Pair<Int?, Int?> {
        return when (layoutComponent) {
            LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE -> R.drawable.button_fast_forward to R.drawable.button_fast_forward_disabled
            LayoutComponent.BUTTON_FAST_FORWARD_1 -> R.drawable.button_fast_forward_1 to R.drawable.button_fast_forward_1_disabled
            LayoutComponent.BUTTON_FAST_FORWARD_2 -> R.drawable.button_fast_forward_2 to R.drawable.button_fast_forward_2_disabled
            LayoutComponent.BUTTON_FAST_FORWARD_3 -> R.drawable.button_fast_forward_3 to R.drawable.button_fast_forward_3_disabled
            LayoutComponent.BUTTON_MICROPHONE_TOGGLE -> R.drawable.button_microphone to R.drawable.button_microphone_disabled
            LayoutComponent.BUTTON_TOGGLE_SOFT_INPUT -> R.drawable.ic_touch_enabled to R.drawable.ic_touch_disabled
            else -> null to null
        }
    }
}