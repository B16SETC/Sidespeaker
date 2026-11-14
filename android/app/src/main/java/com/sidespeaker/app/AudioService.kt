package com.sidespeaker.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.Process
import java.io.IOException
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

private class Ring(private val cap: Int) {
    private val a = ShortArray(cap)
    private var r = 0
    private var w = 0
    private var n = 0

    @Synchronized
    fun count(): Int = n

    @Synchronized
    fun put(src: ShortArray, len: Int) {
        for (i in 0 until len) {
            a[w] = src[i]
            w = (w + 1) % cap
        }
        n += len
    }

    @Synchronized
    fun take(dst: ShortArray, len: Int) {
        for (i in 0 until len) {
            dst[i] = a[r]
            r = (r + 1) % cap
        }
        n -= len
    }
}

class AudioService : Service() {
    companion object {
        @Volatile var status = "Stopped"
        @Volatile var running = false
        private const val CHANNEL = "sidespeaker"
    }

    private val lock = Any()
    @Volatile private var current: Socket? = null
    private var server: ServerSocket? = null
    private var wake: PowerManager.WakeLock? = null
    private var port = 5000
    private var prebufferMs = 80
    private var maxBufferMs = 250
    private var timeoutMs = 3000
    private var trackMult = 2

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY

    override fun onCreate() {
        super.onCreate()
        port = resources.getInteger(R.integer.port)
        prebufferMs = resources.getInteger(R.integer.prebuffer_ms)
        maxBufferMs = resources.getInteger(R.integer.max_buffer_ms)
        timeoutMs = resources.getInteger(R.integer.timeout_ms)
        trackMult = resources.getInteger(R.integer.track_buffer_mult)
        running = true
        startInForeground()
        wake = (getSystemService(POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "sidespeaker:audio")
            .also { it.acquire() }
        Thread({ serve() }, "ss-accept").start()
    }

    override fun onDestroy() {
        running = false
        runCatching { server?.close() }
        synchronized(lock) {
            runCatching { current?.close() }
            current = null
        }
        wake?.let { if (it.isHeld) it.release() }
        status = "Stopped"
        super.onDestroy()
    }

    private fun startInForeground() {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Sidespeaker", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val n = Notification.Builder(this, CHANNEL)
            .setContentTitle("Sidespeaker")
            .setContentText("Playing PC audio over USB")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(1, n)
        }
    }

    private fun serve() {
        try {
            val ss = ServerSocket()
            ss.reuseAddress = true
            ss.bind(InetSocketAddress(port))
            server = ss
            status = "Waiting for PC on port $port"
            while (running) {
                val s = ss.accept()
                if (!s.inetAddress.isLoopbackAddress) {
                    s.close()
                    continue
                }
                synchronized(lock) {
                    runCatching { current?.close() }
                    current = s
                }
                Thread({ handle(s) }, "ss-conn").start()
            }
        } catch (e: Exception) {
            if (running) status = "Error: ${e.message}"
        }
    }

    private fun handle(s: Socket) {
        var track: AudioTrack? = null
        var audio: Thread? = null
        try {
            s.tcpNoDelay = true
            s.soTimeout = timeoutMs
            s.getOutputStream().apply {
                write('S'.code)
                flush()
            }
            val inp = s.getInputStream()
            val hdr = ByteArray(8)
            readFully(inp, hdr)
            val hb = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN)
            val rate = hb.int
            val ch = hb.int
            if (rate !in 8000..192000 || ch !in 1..2) return

            val mask = if (ch == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
            val minBuf = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
            val t = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(rate)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(mask)
                        .build()
                )
                .setBufferSizeInBytes(minBuf * trackMult)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
            track = t
            t.play()

            val prebufS = rate * prebufferMs / 1000 * ch
            val maxS = rate * maxBufferMs / 1000 * ch
            val ring = Ring(maxS + 8192)
            status = "Connected, $rate Hz"

            val at = Thread({ pump(s, t, ring, rate, ch, prebufS) }, "ss-audio")
            at.start()
            audio = at

            val buf = ByteArray(8192)
            val samples = ShortArray(buf.size / 2)
            val frameBytes = 2 * ch
            var carry = 0
            while (running && current === s) {
                val n = inp.read(buf, carry, buf.size - carry)
                if (n < 0) break
                val total = carry + n
                val use = total - total % frameBytes
                if (use > 0) {
                    val cnt = use / 2
                    ByteBuffer.wrap(buf, 0, use).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(samples, 0, cnt)
                    if (ring.count() + cnt <= maxS) ring.put(samples, cnt)
                }
                carry = total - use
                if (carry > 0) System.arraycopy(buf, use, buf, 0, carry)
            }
        } catch (e: Exception) {
        } finally {
            runCatching { s.close() }
            audio?.let { runCatching { it.join(1000) } }
            track?.let {
                runCatching { it.stop() }
                runCatching { it.release() }
            }
            synchronized(lock) {
                if (current === s) {
                    current = null
                    if (running) status = "Waiting for PC on port $port"
                }
            }
        }
    }

    private fun pump(s: Socket, t: AudioTrack, ring: Ring, rate: Int, ch: Int, prebufS: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val chunk = rate / 100 * ch
        val out = ShortArray(chunk)
        var started = false
        var underruns = 0
        var lastStatus = 0L
        while (running && current === s) {
            val have = ring.count()
            if (!started) {
                if (have < prebufS) {
                    Thread.sleep(2)
                    continue
                }
                started = true
            }
            if (have < chunk) {
                started = false
                underruns++
                continue
            }
            ring.take(out, chunk)
            if (t.write(out, 0, chunk) < 0) break
            val now = System.nanoTime()
            if (now - lastStatus > 500_000_000L) {
                lastStatus = now
                val ms = ring.count() * 1000L / (rate * ch)
                status = "Streaming $rate Hz\nbuffer $ms ms · underruns $underruns"
            }
        }
    }

    private fun readFully(inp: InputStream, b: ByteArray) {
        var o = 0
        while (o < b.size) {
            val n = inp.read(b, o, b.size - o)
            if (n < 0) throw IOException("closed")
            o += n
        }
    }
}
