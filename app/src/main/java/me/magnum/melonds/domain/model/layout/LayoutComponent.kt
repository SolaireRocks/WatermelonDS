package me.magnum.melonds.domain.model.layout

import me.magnum.melonds.domain.model.Input

/**
 * @param allowsSharedInputs whether this component may coexist in a layout with other components that map to the same inputs (turbo and combo buttons).
 */
enum class LayoutComponent(val matchingInputs: List<Input>, val allowsSharedInputs: Boolean = false) {
    TOP_SCREEN(emptyList()),
    BOTTOM_SCREEN(emptyList()),
    HYBRID_SCREEN(emptyList()),
    DPAD(listOf(Input.UP, Input.DOWN, Input.LEFT, Input.RIGHT)),
    DPAD_DIAGONAL(listOf(Input.UP, Input.DOWN, Input.LEFT, Input.RIGHT)),
    BUTTONS(listOf(Input.A, Input.B, Input.X, Input.Y)),
    BUTTON_START(listOf(Input.START)),
    BUTTON_SELECT(listOf(Input.SELECT)),
    BUTTON_L(listOf(Input.L)),
    BUTTON_R(listOf(Input.R)),
    BUTTON_HINGE(listOf(Input.HINGE)),
    BUTTON_FAST_FORWARD_TOGGLE(listOf(Input.FAST_FORWARD)),
    BUTTON_FAST_FORWARD_1(listOf(Input.FAST_FORWARD_1)),
    BUTTON_FAST_FORWARD_2(listOf(Input.FAST_FORWARD_2)),
    BUTTON_FAST_FORWARD_3(listOf(Input.FAST_FORWARD_3)),
    BUTTON_TOGGLE_SOFT_INPUT(listOf(Input.TOGGLE_SOFT_INPUT)),
    BUTTON_RESET(listOf(Input.RESET)),
    BUTTON_PAUSE(listOf(Input.PAUSE)),
    BUTTON_SWAP_SCREENS(listOf(Input.SWAP_SCREENS)),
    BUTTON_QUICK_SAVE(listOf(Input.QUICK_SAVE)),
    BUTTON_QUICK_LOAD(listOf(Input.QUICK_LOAD)),
    BUTTON_REWIND(listOf(Input.REWIND)),
    BUTTON_MICROPHONE_TOGGLE(listOf(Input.MICROPHONE)),
    BUTTON_A(listOf(Input.A)),
    BUTTON_B(listOf(Input.B)),
    BUTTON_X(listOf(Input.X)),
    BUTTON_Y(listOf(Input.Y)),
    BUTTON_A_TURBO(listOf(Input.A), allowsSharedInputs = true),
    BUTTON_B_TURBO(listOf(Input.B), allowsSharedInputs = true),
    BUTTON_X_TURBO(listOf(Input.X), allowsSharedInputs = true),
    BUTTON_Y_TURBO(listOf(Input.Y), allowsSharedInputs = true),
    BUTTON_COMBO_A_B(listOf(Input.A, Input.B), allowsSharedInputs = true),
    BUTTON_COMBO_A_X(listOf(Input.A, Input.X), allowsSharedInputs = true),
    BUTTON_COMBO_A_Y(listOf(Input.A, Input.Y), allowsSharedInputs = true),
    BUTTON_COMBO_A_L(listOf(Input.A, Input.L), allowsSharedInputs = true),
    BUTTON_COMBO_A_R(listOf(Input.A, Input.R), allowsSharedInputs = true),
    BUTTON_COMBO_A_START(listOf(Input.A, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_A_SELECT(listOf(Input.A, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_B_X(listOf(Input.B, Input.X), allowsSharedInputs = true),
    BUTTON_COMBO_B_Y(listOf(Input.B, Input.Y), allowsSharedInputs = true),
    BUTTON_COMBO_B_L(listOf(Input.B, Input.L), allowsSharedInputs = true),
    BUTTON_COMBO_B_R(listOf(Input.B, Input.R), allowsSharedInputs = true),
    BUTTON_COMBO_B_START(listOf(Input.B, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_B_SELECT(listOf(Input.B, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_X_Y(listOf(Input.X, Input.Y), allowsSharedInputs = true),
    BUTTON_COMBO_X_L(listOf(Input.X, Input.L), allowsSharedInputs = true),
    BUTTON_COMBO_X_R(listOf(Input.X, Input.R), allowsSharedInputs = true),
    BUTTON_COMBO_X_START(listOf(Input.X, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_X_SELECT(listOf(Input.X, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_Y_L(listOf(Input.Y, Input.L), allowsSharedInputs = true),
    BUTTON_COMBO_Y_R(listOf(Input.Y, Input.R), allowsSharedInputs = true),
    BUTTON_COMBO_Y_START(listOf(Input.Y, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_Y_SELECT(listOf(Input.Y, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_L_R(listOf(Input.L, Input.R), allowsSharedInputs = true),
    BUTTON_COMBO_L_START(listOf(Input.L, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_L_SELECT(listOf(Input.L, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_R_START(listOf(Input.R, Input.START), allowsSharedInputs = true),
    BUTTON_COMBO_R_SELECT(listOf(Input.R, Input.SELECT), allowsSharedInputs = true),
    BUTTON_COMBO_START_SELECT(listOf(Input.START, Input.SELECT), allowsSharedInputs = true),
    SLOT2_ANALOG(emptyList());

    fun isScreen(): Boolean {
        return this == TOP_SCREEN || this == BOTTOM_SCREEN || this == HYBRID_SCREEN
    }

    fun supportsButtonMode(): Boolean {
        return this == BUTTON_A || this == BUTTON_B || this == BUTTON_X || this == BUTTON_Y
    }

    fun isTurboButton(): Boolean {
        return this == BUTTON_A_TURBO || this == BUTTON_B_TURBO || this == BUTTON_X_TURBO || this == BUTTON_Y_TURBO
    }

    fun isComboButton(): Boolean {
        return name.startsWith("BUTTON_COMBO_")
    }

    /**
     * The 1-based fast-forward speed slot controlled by this component, or null if it isn't a fast-forward speed button.
     */
    fun fastForwardSlot(): Int? {
        return when (this) {
            BUTTON_FAST_FORWARD_1 -> 1
            BUTTON_FAST_FORWARD_2 -> 2
            BUTTON_FAST_FORWARD_3 -> 3
            else -> null
        }
    }

    companion object {
        /**
         * The buttons that can be combined into a multi-button component, in the order used to name combo components.
         */
        val COMBO_BUTTON_INPUTS = listOf(Input.A, Input.B, Input.X, Input.Y, Input.L, Input.R, Input.START, Input.SELECT)

        fun comboOf(first: Input, second: Input): LayoutComponent? {
            return entries.firstOrNull { it.isComboButton() && it.matchingInputs.size == 2 && it.matchingInputs.containsAll(listOf(first, second)) }
        }
    }
}
