package com.eruptionlabs.engine;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

// FileProvider escrito a mao: o build do APK e' javac + d8 direto (sem
// Gradle/AndroidX), entao androidx.core.content.FileProvider nao esta'
// disponivel. So' serve o que o motor exporta - getFilesDir()/export/<nome>,
// um nivel so', sem subpasta - e so' leitura. A autoridade e' <pacote>.fileprovider
// e casa com o <provider> do AndroidManifest.xml (tools/android/build_apk.sh): sai
// do pacote porque dois APKs com ids diferentes instalados lado a lado (o app do
// motor e um jogo) nao podem declarar a mesma (INSTALL_FAILED_CONFLICTING_PROVIDER).
public class EruptionFileProvider extends ContentProvider {
    // A autoridade que o manifesto declarou para este provider (lida do
    // PackageManager, entao vale qualquer que seja o jeito de gerar o
    // manifesto); <pacote>.fileprovider se nao a achar.
    public static String authority(android.content.Context context) {
        try {
            android.content.pm.PackageInfo info = context.getPackageManager()
                .getPackageInfo(context.getPackageName(), android.content.pm.PackageManager.GET_PROVIDERS);
            if (info.providers != null) {
                for (android.content.pm.ProviderInfo p : info.providers) {
                    if (EruptionFileProvider.class.getName().equals(p.name) && p.authority != null)
                        return p.authority.split(";")[0];
                }
            }
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            // cai no padrao
        }
        return context.getPackageName() + ".fileprovider";
    }

    private File exportDir;

    @Override
    public boolean onCreate() {
        exportDir = new File(getContext().getFilesDir(), "export");
        return true;
    }

    // uri = content://AUTHORITY/export/<nome>; qualquer outra coisa (ou um
    // nome com separador de pasta, tentando escapar de export/) e' nula.
    private File resolve(Uri uri) {
        java.util.List<String> seg = uri.getPathSegments();
        if (seg.size() != 2 || !"export".equals(seg.get(0))) return null;
        String name = seg.get(1);
        if (name.isEmpty() || name.contains("/") || name.contains("..")) return null;
        File f = new File(exportDir, name);
        try {
            String canon = f.getCanonicalPath();
            String dirCanon = exportDir.getCanonicalPath();
            if (!canon.startsWith(dirCanon + File.separator)) return null;
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = resolve(uri);
        if (f == null || !f.isFile()) throw new FileNotFoundException(String.valueOf(uri));
        // Sempre leitura: quem recebe o compartilhamento nunca grava de volta.
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = resolve(uri);
        if (f == null || !f.isFile()) return null;
        String[] cols = {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor cursor = new MatrixCursor(cols);
        cursor.addRow(new Object[]{f.getName(), f.length()});
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        File f = resolve(uri);
        if (f == null) return null;
        int dot = f.getName().lastIndexOf('.');
        String ext = dot >= 0 ? f.getName().substring(dot + 1) : "";
        String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        return type != null ? type : "application/octet-stream";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("EruptionFileProvider e' so' leitura");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("EruptionFileProvider e' so' leitura");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("EruptionFileProvider e' so' leitura");
    }
}
