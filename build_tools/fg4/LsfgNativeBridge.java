package com.movtery.zalithlauncher.framegen;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class LsfgNativeBridge {
    static {
        System.loadLibrary("zalith_lsfg");
    }

    public static final int STATUS_OK = 0;
    public static final int STATUS_NOT_INSTALLED = 1;
    public static final int STATUS_UNREADABLE_FILE = 2;
    public static final int STATUS_NOT_PORTABLE_EXECUTABLE = 3;
    public static final int STATUS_MISSING_SHADERS = 4;
    public static final int STATUS_TRANSLATION_FAILED = 5;
    public static final int STATUS_CACHE_UNUSABLE = 6;

    private LsfgNativeBridge() {}

    public static native int nativeValidateDll(String path);
    public static native int nativeBuildCache(String dllPath, String cachePath, boolean preferFp16);
    public static native boolean nativeCacheMatches(String cachePath, String dllPath);
    public static native String nativeStatusName(int status);

    public static File dllFile(Context context) {
        return new File(context.getFilesDir(), "lsfg-vk/Lossless.dll");
    }

    public static File cacheFile(Context context) {
        return new File(context.getFilesDir(), "lsfg-native/shaders.cache");
    }

    public static boolean hasDll(Context context) {
        return dllFile(context).isFile();
    }

    public static boolean isReady(Context context) {
        File dll = dllFile(context);
        File cache = cacheFile(context);
        return dll.isFile() && cache.isFile()
                && nativeCacheMatches(cache.getAbsolutePath(), dll.getAbsolutePath());
    }

    public static int importDll(Context context, Uri uri) {
        if (uri == null) return STATUS_NOT_INSTALLED;

        File destination = dllFile(context);
        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return STATUS_CACHE_UNUSABLE;
        }

        File temp = new File(parent, "Lossless.dll.importing");
        try (InputStream in = context.getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(temp, false)) {
            if (in == null) return STATUS_UNREADABLE_FILE;
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) out.write(buffer, 0, read);
            out.getFD().sync();
        } catch (Exception e) {
            temp.delete();
            return STATUS_UNREADABLE_FILE;
        }

        int validation = nativeValidateDll(temp.getAbsolutePath());
        if (validation != STATUS_OK) {
            temp.delete();
            return validation;
        }

        if (destination.exists() && !destination.delete()) {
            temp.delete();
            return STATUS_CACHE_UNUSABLE;
        }
        if (!temp.renameTo(destination)) {
            temp.delete();
            return STATUS_CACHE_UNUSABLE;
        }

        File cache = cacheFile(context);
        File cacheParent = cache.getParentFile();
        if (cacheParent != null && !cacheParent.isDirectory() && !cacheParent.mkdirs()) {
            return STATUS_CACHE_UNUSABLE;
        }
        if (cache.exists()) cache.delete();

        return nativeBuildCache(
                destination.getAbsolutePath(),
                cache.getAbsolutePath(),
                true
        );
    }

    public static int ensureCache(Context context) {
        File dll = dllFile(context);
        if (!dll.isFile()) return STATUS_NOT_INSTALLED;

        File cache = cacheFile(context);
        if (cache.isFile()
                && nativeCacheMatches(cache.getAbsolutePath(), dll.getAbsolutePath())) {
            return STATUS_OK;
        }

        File parent = cache.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return STATUS_CACHE_UNUSABLE;
        }

        return nativeBuildCache(
                dll.getAbsolutePath(),
                cache.getAbsolutePath(),
                true
        );
    }

    public static String explain(int status) {
        switch (status) {
            case STATUS_OK:
                return "LSFG Native pronto";
            case STATUS_NOT_INSTALLED:
                return "Importe sua Lossless.dll";
            case STATUS_UNREADABLE_FILE:
                return "Não foi possível ler a Lossless.dll";
            case STATUS_NOT_PORTABLE_EXECUTABLE:
                return "O arquivo selecionado não é uma DLL PE válida";
            case STATUS_MISSING_SHADERS:
                return "Essa Lossless.dll não contém a cadeia de shaders LSFG compatível";
            case STATUS_TRANSLATION_FAILED:
                return "Falha ao converter os shaders LSFG para SPIR-V";
            case STATUS_CACHE_UNUSABLE:
                return "Não foi possível criar o cache LSFG";
            default:
                return "Erro LSFG: " + nativeStatusName(status);
        }
    }
}
