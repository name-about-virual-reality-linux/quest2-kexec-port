package com.qkx.installer

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import com.google.android.material.color.MaterialColors
import kotlin.math.max
import kotlin.math.min

/**
 * Material3 split bar: thick rounded track, Arch fill from the left in
 * primary, Horizon fill from the right in secondary-container, divided by
 * a draggable thumb knob. Starts dead center.
 */
class SplitBar @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    /** 0..1 share for Arch. Starts dead center. */
    var fraction: Float = 0.5f
        private set
    var minFraction: Float = 0.05f
    var maxFraction: Float = 0.95f
    var onSplit: ((Float) -> Unit)? = null

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val archPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val horizonPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeCap = Paint.Cap.ROUND
    }
    private val knobPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val knobShadow = Paint(Paint.ANTI_ALIAS_FLAG)

    private val rect = RectF()
    private var dragging = false
    private var colorsInit = false

    private fun initColors() {
        if (colorsInit) return
        colorsInit = true
        val def = 0xFF2E2E35.toInt()
        trackPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorSurfaceContainerHighest, def)
        archPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorPrimary, 0xFF7DD3FC.toInt())
        horizonPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorSecondaryContainer, 0xFFC4B5FD.toInt())
        linePaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnPrimaryContainer, 0xFFFFFFFF.toInt())
        knobPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorPrimaryContainer, 0xFFFFFFFF.toInt())
        knobShadow.color = 0x40000000
    }

    fun setFractionClamped(f: Float) {
        val nf = min(maxFraction, max(minFraction, f))
        if (nf != fraction) {
            fraction = nf
            onSplit?.invoke(nf)
            invalidate()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val h = (64 * resources.displayMetrics.density).toInt()
        setMeasuredDimension(
            MeasureSpec.getSize(widthMeasureSpec),
            resolveSize(h, heightMeasureSpec),
        )
    }

    override fun onDraw(canvas: Canvas) {
        initColors()
        val w = width.toFloat()
        val h = height.toFloat()
        val trackH = h * 0.45f
        val top = (h - trackH) / 2f
        val r = trackH / 2f
        // Base track.
        rect.set(0f, top, w, top + trackH)
        canvas.drawRoundRect(rect, r, r, trackPaint)
        val splitX = w * fraction
        // Arch fill (left), squared inner edge at the divider.
        if (splitX > 0) {
            canvas.save()
            canvas.clipRect(0f, top, splitX, top + trackH)
            rect.set(0f, top, w, top + trackH)
            canvas.drawRoundRect(rect, r, r, archPaint)
            canvas.restore()
        }
        // Horizon fill (right).
        if (splitX < w) {
            canvas.save()
            canvas.clipRect(splitX, top, w, top + trackH)
            rect.set(0f, top, w, top + trackH)
            canvas.drawRoundRect(rect, r, r, horizonPaint)
            canvas.restore()
        }
        val cy = h / 2f
        // Thin divider line through the track.
        linePaint.strokeWidth = 6f
        canvas.drawLine(splitX, top + 2f, splitX, top + trackH - 2f, linePaint)
        // Bar handle: short rounded vertical bar slightly taller than the
        // track, overlapping both sides.
        val density = resources.displayMetrics.density
        val barW = 16f * density
        val overhang = 9f * density
        rect.set(splitX - barW / 2f, top - overhang, splitX + barW / 2f, top + trackH + overhang)
        canvas.drawRoundRect(rect, barW / 2f, barW / 2f, knobShadow)
        rect.set(splitX - barW / 2f, top - overhang + 3f, splitX + barW / 2f, top + trackH + overhang)
        canvas.drawRoundRect(rect, barW / 2f, barW / 2f, knobPaint)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                dragging = true
                parent?.requestDisallowInterceptTouchEvent(true)
                setFractionClamped(event.x / width)
                return true
            }
            MotionEvent.ACTION_MOVE -> if (dragging) {
                setFractionClamped(event.x / width)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragging = false
                parent?.requestDisallowInterceptTouchEvent(false)
                return true
            }
        }
        return super.onTouchEvent(event)
    }
}
