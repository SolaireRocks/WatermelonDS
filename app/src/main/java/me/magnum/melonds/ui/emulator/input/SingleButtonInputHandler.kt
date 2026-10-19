package me.magnum.melonds.ui.emulator.input

import android.view.MotionEvent
import android.view.View
import me.magnum.melonds.common.vibration.TouchVibrator
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.layout.VirtualButtonMode

/**
 * Handles a virtual button that drives one or more inputs at the same time (more than one input is used by multi-button combos).
 */
class SingleButtonInputHandler(inputListener: IInputListener, private val inputs: List<Input>, enableHapticFeedback: Boolean, touchVibrator: TouchVibrator, private val mode: VirtualButtonMode = VirtualButtonMode.NORMAL) : FeedbackInputHandler(inputListener, enableHapticFeedback, touchVibrator) {
    constructor(inputListener: IInputListener, input: Input, enableHapticFeedback: Boolean, touchVibrator: TouchVibrator, mode: VirtualButtonMode = VirtualButtonMode.NORMAL) : this(inputListener, listOf(input), enableHapticFeedback, touchVibrator, mode)

    private var pressed = false
    private var touchDown = false
    private var activeView: View? = null
    private val turboPulse = object : Runnable {
        override fun run() {
            val view = activeView ?: return
            if (!touchDown) return
            setPressed(view, !pressed)
            view.postDelayed(this, 50L)
        }
    }

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (touchDown) return true
                touchDown = true
                activeView = v
                setPressed(v, if (mode == VirtualButtonMode.TOGGLE) !pressed else true)
                if (mode == VirtualButtonMode.TURBO) v.postDelayed(turboPulse, 50L)
                performHapticFeedback(v, HapticFeedbackType.KEY_PRESS)
            }
            MotionEvent.ACTION_UP -> {
                touchDown = false
                if (mode != VirtualButtonMode.TOGGLE) release()
                performHapticFeedback(v, HapticFeedbackType.KEY_RELEASE)
            }
            MotionEvent.ACTION_CANCEL -> release()
        }
        return true
    }

    fun release() {
        touchDown = false
        activeView?.let {
            it.removeCallbacks(turboPulse)
            setPressed(it, false)
        }
        activeView = null
    }

    private fun setPressed(view: View, value: Boolean) {
        if (pressed == value) return
        pressed = value
        view.isPressed = value
        inputs.forEach { if (value) inputListener.onKeyPress(it) else inputListener.onKeyReleased(it) }
    }
}
