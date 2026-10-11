package com.eruptionlabs.engine;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

// NativeActivity em tela cheia imersiva: barra de status e de navegacao
// escondidas (voltam com um deslize da borda e somem de novo). E' o unico
// codigo Java do APK; o jogo continua todo no liberuption.so.
public class EruptionActivity extends NativeActivity {
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    // Voltar nao fecha o jogo (o toque acidental na barra encerrava a demo).
    @Override
    public void onBackPressed() {}
}
