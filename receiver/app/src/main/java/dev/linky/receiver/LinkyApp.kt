package dev.linky.receiver

import android.app.Application
import android.util.Log

class LinkyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Log.i("LinkyApp", "Linky application initialized")
    }
}
