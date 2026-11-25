package com.eruptionlabs.engine;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.inputmethod.InputMethodManager;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.IOException;

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
        // Jogo: a tela nao apaga no meio da partida (sem toque por minutos, ex.: so' camera).
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    // Teclado do sistema para input.text_begin/text_end (chamado por JNI de
    // AndroidMain.cpp). A NativeActivity nao tem campo de texto: o teclado e'
    // aberto sobre a view da janela e as teclas dele chegam ao codigo nativo
    // como eventos AKEY (unicodeOf converte cada uma no caractere).
    public void setTextInput(final boolean on) {
        runOnUiThread(() -> {
            try {
                final View v = getWindow().getDecorView();
                final InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (on) {
                    v.setFocusable(true);
                    v.setFocusableInTouchMode(true);
                    v.requestFocus();
                    imm.showSoftInput(v, InputMethodManager.SHOW_FORCED);
                } else {
                    imm.hideSoftInputFromWindow(v.getWindowToken(), 0);
                }
            } catch (RuntimeException e) {
                Log.e("Eruption", "setTextInput falhou: " + e);
            }
        });
    }

    // Caractere de uma tecla com a camada (shift, acento) em 'meta'; 0 se a
    // tecla nao produz texto.
    public int unicodeOf(int keyCode, int meta) {
        return new KeyEvent(KeyEvent.ACTION_DOWN, keyCode).getUnicodeChar(meta);
    }

    // Voltar nao fecha o jogo (o toque acidental na barra encerrava a demo).
    @Override
    public void onBackPressed() {}

    // Chamado pela engine nativa (JNI, src/platform/android/AndroidMain.cpp)
    // para abrir o share sheet do sistema com um arquivo que o jogo gerou
    // (ex.: telemetry.export). O Android nao deixa outro app abrir um
    // caminho de arquivo direto: so' um content://, servido aqui pelo
    // EruptionFileProvider - o unico arquivo que ele serve e' absPath dentro
    // de getFilesDir()/export (telemetry.export so' grava ali). true se a
    // validacao passou e o pedido de abrir o share sheet foi mandado pro
    // thread de UI (startActivity precisa dele); falhas depois disso so'
    // aparecem no logcat, a engine ja' nao espera por elas.
    public boolean shareFile(String absPath, String mimeType) {
        final File f, exportDir;
        try {
            f = new File(absPath).getCanonicalFile();
            exportDir = new File(getFilesDir(), "export").getCanonicalFile();
        } catch (IOException e) {
            Log.e("Eruption", "shareFile: caminho invalido: " + e);
            return false;
        }
        if (!f.getPath().startsWith(exportDir.getPath() + File.separator) || !f.isFile()) {
            Log.e("Eruption", "shareFile: fora da pasta de export: " + absPath);
            return false;
        }
        final String mime = mimeType == null || mimeType.isEmpty() ? "application/octet-stream" : mimeType;
        runOnUiThread(() -> {
            try {
                Uri uri = Uri.parse("content://" + EruptionFileProvider.authority(this)
                    + "/export/" + Uri.encode(f.getName()));
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType(mime);
                send.putExtra(Intent.EXTRA_STREAM, uri);
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(Intent.createChooser(send, f.getName()));
            } catch (SecurityException | android.content.ActivityNotFoundException e) {
                Log.e("Eruption", "shareFile falhou: " + e);
            }
        });
        return true;
    }
}
