package me.magnum.melonds.utils

import android.content.res.Resources
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.layout.LayoutComponent

fun getLayoutComponentName(resources: Resources, layoutComponent: LayoutComponent): String {
    layoutComponent.fastForwardSlot()?.let {
        return resources.getString(R.string.input_fast_forward_slot, it)
    }
    if (layoutComponent.isComboButton()) {
        return layoutComponent.matchingInputs.joinToString(" + ") { getInputName(resources, it) }
    }
    if (layoutComponent.isTurboButton()) {
        return resources.getString(R.string.input_turbo_format, getInputName(resources, layoutComponent.matchingInputs.single()))
    }
    return resources.getString(getStaticLayoutComponentName(layoutComponent))
}

/**
 * A label short enough to be drawn inside a button.
 */
fun getShortInputLabel(resources: Resources, input: Input): String {
    return when (input) {
        Input.START -> resources.getString(R.string.input_start_short)
        Input.SELECT -> resources.getString(R.string.input_select_short)
        else -> getInputName(resources, input)
    }
}

fun getInputName(resources: Resources, input: Input): String {
    return resources.getString(when (input) {
        Input.A -> R.string.input_a
        Input.B -> R.string.input_b
        Input.X -> R.string.input_x
        Input.Y -> R.string.input_y
        Input.L -> R.string.input_l
        Input.R -> R.string.input_r
        Input.START -> R.string.input_start
        Input.SELECT -> R.string.input_select
        else -> throw IllegalArgumentException("Input $input has no button name")
    })
}

private fun getStaticLayoutComponentName(layoutComponent: LayoutComponent): Int {
    return when (layoutComponent) {
        LayoutComponent.TOP_SCREEN -> R.string.top_screen
        LayoutComponent.BOTTOM_SCREEN -> R.string.bottom_screen
        LayoutComponent.HYBRID_SCREEN -> R.string.hybrid_screen
        LayoutComponent.SLOT2_ANALOG -> R.string.input_slot2_analog
        LayoutComponent.DPAD -> R.string.input_dpad
        LayoutComponent.DPAD_DIAGONAL -> R.string.input_dpad_diagonal
        LayoutComponent.BUTTONS -> R.string.input_abxy_buttons
        LayoutComponent.BUTTON_A -> R.string.input_a
        LayoutComponent.BUTTON_B -> R.string.input_b
        LayoutComponent.BUTTON_X -> R.string.input_x
        LayoutComponent.BUTTON_Y -> R.string.input_y
        LayoutComponent.BUTTON_L -> R.string.input_l
        LayoutComponent.BUTTON_R -> R.string.input_r
        LayoutComponent.BUTTON_START -> R.string.input_start
        LayoutComponent.BUTTON_SELECT -> R.string.input_select
        LayoutComponent.BUTTON_HINGE -> R.string.input_lid
        LayoutComponent.BUTTON_PAUSE -> R.string.input_pause
        LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE -> R.string.input_fast_forward
        LayoutComponent.BUTTON_MICROPHONE_TOGGLE -> R.string.input_microphone
        LayoutComponent.BUTTON_TOGGLE_SOFT_INPUT -> R.string.input_toggle_soft_input
        LayoutComponent.BUTTON_RESET -> R.string.reset
        LayoutComponent.BUTTON_SWAP_SCREENS -> R.string.input_swap_screens
        LayoutComponent.BUTTON_QUICK_SAVE -> R.string.input_quick_save
        LayoutComponent.BUTTON_QUICK_LOAD -> R.string.input_quick_load
        LayoutComponent.BUTTON_REWIND -> R.string.rewind
        // Components with generated names are handled by getLayoutComponentName
        else -> throw IllegalArgumentException("Component $layoutComponent has no static name")
    }
}
