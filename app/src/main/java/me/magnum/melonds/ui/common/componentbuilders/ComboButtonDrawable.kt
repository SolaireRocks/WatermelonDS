package me.magnum.melonds.ui.common.componentbuilders

import android.content.Context
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import androidx.core.content.ContextCompat
import me.magnum.melonds.R

/**
 * Draws a round button with a short text label, used for multi-button combos (e.g. "A+B").
 */
class ComboButtonDrawable(context: Context, private val label: String) : Drawable() {
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = ContextCompat.getColor(context, R.color.face_button_fill)
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFFFFFF.toInt()
        typeface = Typeface.DEFAULT_BOLD
        textAlign = Paint.Align.CENTER
    }

    override fun draw(canvas: Canvas) {
        val bounds = bounds
        val radius = bounds.width().coerceAtMost(bounds.height()) / 2f * (31f / 32f)
        val centerX = bounds.exactCenterX()
        val centerY = bounds.exactCenterY()
        canvas.drawCircle(centerX, centerY, radius, fillPaint)

        // Scale the text so that it always fits inside the circle, whatever the label length
        textPaint.textSize = radius
        val maxTextWidth = radius * 1.5f
        val textWidth = textPaint.measureText(label)
        if (textWidth > maxTextWidth) {
            textPaint.textSize = radius * maxTextWidth / textWidth
        }
        val textCenterOffset = (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, centerX, centerY - textCenterOffset, textPaint)
    }

    override fun setAlpha(alpha: Int) {
        fillPaint.alpha = alpha
        textPaint.alpha = alpha
    }

    override fun setColorFilter(colorFilter: ColorFilter?) {
        fillPaint.colorFilter = colorFilter
        textPaint.colorFilter = colorFilter
    }

    @Deprecated("Deprecated in Java")
    override fun getOpacity() = PixelFormat.TRANSLUCENT
}
