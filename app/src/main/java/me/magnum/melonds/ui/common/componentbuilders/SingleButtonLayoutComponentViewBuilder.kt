package me.magnum.melonds.ui.common.componentbuilders

import android.content.Context
import android.view.View
import android.widget.ImageView
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.ui.common.LayoutComponentViewBuilder
import me.magnum.melonds.utils.getShortInputLabel

class SingleButtonLayoutComponentViewBuilder(private val layoutComponent: LayoutComponent) : LayoutComponentViewBuilder() {
    override fun build(context: Context): View {
        return ImageView(context).apply {
            if (layoutComponent.isComboButton()) {
                setImageDrawable(ComboButtonDrawable(context, getComboLabel(context)))
            } else {
                setImageResource(getInputDrawable())
            }
        }
    }

    override fun getAspectRatio() = 1f

    private fun getComboLabel(context: Context): String {
        return layoutComponent.matchingInputs.joinToString("+") { getShortInputLabel(context.resources, it) }
    }

    private fun getInputDrawable(): Int {
        return when (layoutComponent) {
            LayoutComponent.BUTTON_A -> R.drawable.button_a
            LayoutComponent.BUTTON_B -> R.drawable.button_b
            LayoutComponent.BUTTON_X -> R.drawable.button_x
            LayoutComponent.BUTTON_Y -> R.drawable.button_y
            LayoutComponent.BUTTON_A_TURBO -> R.drawable.button_a_turbo
            LayoutComponent.BUTTON_B_TURBO -> R.drawable.button_b_turbo
            LayoutComponent.BUTTON_X_TURBO -> R.drawable.button_x_turbo
            LayoutComponent.BUTTON_Y_TURBO -> R.drawable.button_y_turbo
            LayoutComponent.DPAD_DIAGONAL -> R.drawable.dpad_diagonal
            LayoutComponent.BUTTON_FAST_FORWARD_1 -> R.drawable.button_fast_forward_1
            LayoutComponent.BUTTON_FAST_FORWARD_2 -> R.drawable.button_fast_forward_2
            LayoutComponent.BUTTON_FAST_FORWARD_3 -> R.drawable.button_fast_forward_3
            LayoutComponent.BUTTON_L -> R.drawable.button_l
            LayoutComponent.BUTTON_R -> R.drawable.button_r
            LayoutComponent.BUTTON_START -> R.drawable.button_start
            LayoutComponent.BUTTON_SELECT -> R.drawable.button_select
            LayoutComponent.BUTTON_HINGE -> R.drawable.button_toggle_lid
            LayoutComponent.BUTTON_PAUSE -> R.drawable.button_pause
            LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE -> R.drawable.button_fast_forward
            LayoutComponent.BUTTON_TOGGLE_SOFT_INPUT -> R.drawable.ic_touch_enabled
            LayoutComponent.BUTTON_RESET -> R.drawable.button_reset
            LayoutComponent.BUTTON_SWAP_SCREENS -> R.drawable.button_swap_screens
            LayoutComponent.BUTTON_QUICK_SAVE -> R.drawable.button_quick_save
            LayoutComponent.BUTTON_QUICK_LOAD -> R.drawable.button_quick_load
            LayoutComponent.BUTTON_REWIND -> R.drawable.button_rewind
            LayoutComponent.BUTTON_MICROPHONE_TOGGLE -> R.drawable.button_microphone
            else -> -1
        }
    }
}
