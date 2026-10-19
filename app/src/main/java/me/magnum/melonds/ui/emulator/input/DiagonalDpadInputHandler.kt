package me.magnum.melonds.ui.emulator.input

import android.view.MotionEvent
import android.view.View
import me.magnum.melonds.common.vibration.TouchVibrator
import me.magnum.melonds.domain.model.Input
import kotlin.math.atan2
import kotlin.math.hypot

/**
 * Eight-way D-pad. The touch position relative to the center of the view selects one of eight sectors; the four diagonal sectors press two directions at once.
 * Diagonal sectors are wider than the cardinal ones (50 degrees vs 40) to make diagonal movement easier to hit.
 */
class DiagonalDpadInputHandler(inputListener: IInputListener, enableHapticFeedback: Boolean, touchVibrator: TouchVibrator) : FeedbackInputHandler(inputListener, enableHapticFeedback, touchVibrator) {

    private val pressedInputs = mutableSetOf<Input>()
    private val newPressedInputs = mutableSetOf<Input>()

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        newPressedInputs.clear()

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> getInputsForPosition(v, event.x, event.y, newPressedInputs)
        }

        val released = pressedInputs.filter { it !in newPressedInputs }
        val pressed = newPressedInputs.filter { it !in pressedInputs }
        released.forEach { inputListener.onKeyReleased(it) }
        pressed.forEach { inputListener.onKeyPress(it) }

        if (pressed.isNotEmpty()) {
            performHapticFeedback(v, HapticFeedbackType.KEY_PRESS)
        } else if (released.isNotEmpty() && newPressedInputs.isEmpty()) {
            performHapticFeedback(v, HapticFeedbackType.KEY_RELEASE)
        }

        pressedInputs.clear()
        pressedInputs.addAll(newPressedInputs)
        return true
    }

    fun release() {
        pressedInputs.forEach { inputListener.onKeyReleased(it) }
        pressedInputs.clear()
    }

    private fun getInputsForPosition(view: View, x: Float, y: Float, outInputs: MutableSet<Input>) {
        val dx = x - view.width / 2f
        val dy = y - view.height / 2f
        // Ignore the center of the pad so resting a thumb on it doesn't press anything
        if (hypot(dx, dy) < view.width * DEAD_ZONE_RATIO) return

        // 0 degrees is right and angles grow clockwise because the Y axis points down
        var angle = Math.toDegrees(atan2(dy, dx).toDouble())
        if (angle < 0) angle += 360.0

        when {
            angle < 20 || angle >= 340 -> outInputs.add(Input.RIGHT)
            angle < 70 -> outInputs.addAll(listOf(Input.DOWN, Input.RIGHT))
            angle < 110 -> outInputs.add(Input.DOWN)
            angle < 160 -> outInputs.addAll(listOf(Input.DOWN, Input.LEFT))
            angle < 200 -> outInputs.add(Input.LEFT)
            angle < 250 -> outInputs.addAll(listOf(Input.UP, Input.LEFT))
            angle < 290 -> outInputs.add(Input.UP)
            else -> outInputs.addAll(listOf(Input.UP, Input.RIGHT))
        }
    }

    private companion object {
        const val DEAD_ZONE_RATIO = 0.08f
    }
}
