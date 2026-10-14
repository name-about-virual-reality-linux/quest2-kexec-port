package com.qkx.installer

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.max
import kotlin.math.min

/**
 * Thick rounded split bar: Arch fill grows from the left, Horizon fill
 * shrinks from the right, divided by a draggable center line that starts
 * in the middle. Drag left = less Arch / more Horizon, and vice versa.
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

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF2E2E35.toInt() }
    private val archPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF7DD3FC.toInt() }
    private val horizonPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFC4B5FD.toInt() }
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFFFFFF.toInt()
        strokeWidth = 8f
        strokeCap = Paint.Cap.ROUND
    }

    private val rect = RectF()
    private var dragging = false

    fun setFractionClamped(f: Float) {
        val nf = min(maxFraction, max(minFraction, f))
        if (nf != fraction) {
            fraction = nf
            onSplit?.invoke(nf)
            invalidate()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val h = (56 * resources.displayMetrics.density).toInt()
        setMeasuredDimension(
            MeasureSpec.getSize(widthMeasureSpec),
            resolveSize(h, heightMeasureSpec),
        )
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val r = h / 2f // fully rounded ends
        // Base track.
        rect.set(0f, 0f, w, h)
        canvas.drawRoundRect(rect, r, r, trackPaint)
        val splitX = w * fraction
        // Arch fill (left), squared inner edge at the divider.
        if (splitX > 0) {
            canvas.save()
            canvas.clipRect(0f, 0f, splitX, h)
            rect.set(0f, 0f, w, h)
            canvas.drawRoundRect(rect, r, r, archPaint)
            canvas.restore()
        }
        // Horizon fill (right).
        if (splitX < w) {
            canvas.save()
            canvas.clipRect(splitX, 0f, w, h)
            rect.set(0f, 0f, w, h)
            canvas.drawRoundRect(rect, r, r, horizonPaint)
            canvas.restore()
        }
        // Divider line.
        canvas.drawLine(splitX, 8f, splitX, h - 8f, linePaint)
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
