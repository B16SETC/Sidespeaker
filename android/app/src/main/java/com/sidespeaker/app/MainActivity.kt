package com.sidespeaker.app

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var statusView: TextView
    private lateinit var toggle: Button

    private val tick = object : Runnable {
        override fun run() {
            statusView.text = AudioService.status
            toggle.text = if (AudioService.running) "Stop" else "Start"
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }

        val pad = (24 * resources.displayMetrics.density).toInt()
        val title = TextView(this).apply {
            text = "Sidespeaker"
            textSize = 32f
            gravity = Gravity.CENTER
        }
        statusView = TextView(this).apply {
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(0, pad, 0, pad)
        }
        toggle = Button(this).apply {
            setOnClickListener { toggleService() }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(pad, pad, pad, pad)
            addView(title)
            addView(statusView)
            addView(toggle)
        })

        if (!AudioService.running) startForegroundService(Intent(this, AudioService::class.java))
    }

    override fun onResume() {
        super.onResume()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    private fun toggleService() {
        val i = Intent(this, AudioService::class.java)
        if (AudioService.running) stopService(i) else startForegroundService(i)
    }
}
