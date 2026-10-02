package dev.framegen.android;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.Locale;

public final class LosslessDllManager {
    private static final String DIRECTORY = "framegen";
    private static final String FILE_NAME = "Lossless.dll";

    private LosslessDllManager() {}

    public static final class Info {
        public final boolean installed;
        public final boolean validPe;
        public final boolean x64;
        public final long size;
        public final String sha256;
        public final String sourceName;
        public final String message;

        private Info(
                boolean installed,
                boolean validPe,
                boolean x64,
                long size,
                String sha256,
                String sourceName,
                String message) {
            this.installed = installed;
            this.validPe = validPe;
            this.x64 = x64;
            this.size = size;
            this.sha256 = sha256;
            this.sourceName = sourceName;
            this.message = message;
        }

        public String describe() {
            if (!installed) {
                return "Lossless.dll: não adicionada.";
            }

            StringBuilder out = new StringBuilder();
            out.append("Lossless.dll adicionada\n");
            out.append("Formato: ")
                    .append(validPe ? "PE32+ Windows" : "arquivo não reconhecido")
                    .append("\n");
            out.append("Arquitetura: ")
                    .append(x64 ? "x86-64" : "não-x86-64")
                    .append("\n");
            out.append("Tamanho: ")
                    .append(String.format(
                            Locale.US,
                            "%.2f MB",
                            size / 1024.0 / 1024.0))
                    .append("\n");
            if (sha256 != null && !sha256.isEmpty()) {
                out.append("SHA-256: ").append(sha256).append("\n");
            }
            out.append(message);
            return out.toString();
        }
    }

    public static Info importFromUri(Context context, Uri uri) throws Exception {
        if (uri == null) {
            throw new IllegalArgumentException("URI da DLL é nula.");
        }

        File directory = new File(context.getFilesDir(), DIRECTORY);
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IllegalStateException(
                    "Não foi possível criar o diretório privado do backend.");
        }

        String sourceName = queryDisplayName(context, uri);
        File temporary = new File(directory, FILE_NAME + ".tmp");
        File destination = new File(directory, FILE_NAME);

        try (InputStream in = context.getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(temporary, false)) {
            if (in == null) {
                throw new IllegalStateException(
                        "Não foi possível abrir o arquivo selecionado.");
            }

            byte[] buffer = new byte[64 * 1024];
            int read;
            long total = 0;
            final long maximum = 256L * 1024L * 1024L;

            while ((read = in.read(buffer)) != -1) {
                total += read;
                if (total > maximum) {
                    throw new IllegalArgumentException(
                            "DLL maior que o limite de 256 MB.");
                }
                out.write(buffer, 0, read);
            }
            out.getFD().sync();
        } catch (Throwable t) {
            temporary.delete();
            throw t;
        }

        Info candidate = inspectFile(temporary, sourceName);
        if (!candidate.validPe) {
            temporary.delete();
            throw new IllegalArgumentException(
                    "O arquivo não é uma DLL PE válida do Windows.");
        }

        if (!candidate.x64) {
            temporary.delete();
            throw new IllegalArgumentException(
                    "A DLL precisa ser x86-64 para o backend Lossless atual.");
        }

        if (destination.exists() && !destination.delete()) {
            temporary.delete();
            throw new IllegalStateException(
                    "Não foi possível substituir a DLL anterior.");
        }

        if (!temporary.renameTo(destination)) {
            temporary.delete();
            throw new IllegalStateException(
                    "Não foi possível mover a DLL para o armazenamento privado.");
        }

        return inspectFile(destination, sourceName);
    }

    public static Info inspect(Context context) {
        File file = getDllFile(context);
        if (!file.exists()) {
            return new Info(
                    false,
                    false,
                    false,
                    0,
                    "",
                    "",
                    "Selecione a DLL do Lossless Scaling.");
        }

        try {
            return inspectFile(file, FILE_NAME);
        } catch (Exception e) {
            return new Info(
                    true,
                    false,
                    false,
                    file.length(),
                    "",
                    FILE_NAME,
                    "Falha ao validar: " + e.getMessage());
        }
    }

    public static boolean remove(Context context) {
        File file = getDllFile(context);
        return !file.exists() || file.delete();
    }

    public static File getDllFile(Context context) {
        return new File(
                new File(context.getFilesDir(), DIRECTORY),
                FILE_NAME);
    }

    private static Info inspectFile(
            File file,
            String sourceName) throws Exception {
        PeInfo pe = inspectPe(file);

        return new Info(
                true,
                pe.valid,
                pe.machine == 0x8664,
                file.length(),
                sha256(file),
                sourceName == null ? FILE_NAME : sourceName,
                pe.machine == 0x8664
                        ? "Backend detectado: Windows x86-64. "
                          + "A DLL está pronta para o futuro runtime PE/DXGI; "
                          + "ela não pode ser carregada por dlopen() no Android ARM64."
                        : "Arquitetura PE não suportada pelo backend planejado.");
    }

    private static final class PeInfo {
        boolean valid;
        int machine;
    }

    private static PeInfo inspectPe(File file) throws Exception {
        PeInfo info = new PeInfo();

        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            if (raf.length() < 0x100) {
                return info;
            }

            if (readU16LE(raf, 0) != 0x5A4D) {
                return info;
            }

            long peOffset = readU32LE(raf, 0x3C);
            if (peOffset <= 0 || peOffset + 26 > raf.length()) {
                return info;
            }

            raf.seek(peOffset);
            if (raf.readUnsignedByte() != 'P' ||
                raf.readUnsignedByte() != 'E' ||
                raf.readUnsignedByte() != 0 ||
                raf.readUnsignedByte() != 0) {
                return info;
            }

            info.machine = readU16LE(raf, peOffset + 4);
            int optionalHeaderSize = readU16LE(raf, peOffset + 20);
            if (optionalHeaderSize < 2 ||
                peOffset + 24L + optionalHeaderSize > raf.length()) {
                return info;
            }

            int optionalMagic = readU16LE(raf, peOffset + 24);
            info.valid = optionalMagic == 0x20B || optionalMagic == 0x10B;
            return info;
        }
    }

    private static int readU16LE(RandomAccessFile raf, long offset)
            throws Exception {
        raf.seek(offset);
        int b0 = raf.readUnsignedByte();
        int b1 = raf.readUnsignedByte();
        return b0 | (b1 << 8);
    }

    private static long readU32LE(RandomAccessFile raf, long offset)
            throws Exception {
        raf.seek(offset);
        long b0 = raf.readUnsignedByte();
        long b1 = raf.readUnsignedByte();
        long b2 = raf.readUnsignedByte();
        long b3 = raf.readUnsignedByte();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");

        try (FileInputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) != -1) {
                digest.update(buffer, 0, read);
            }
        }

        StringBuilder out = new StringBuilder();
        for (byte b : digest.digest()) {
            out.append(String.format(Locale.US, "%02x", b & 0xFF));
        }
        return out.toString();
    }

    private static String queryDisplayName(Context context, Uri uri) {
        Cursor cursor = null;
        try {
            cursor = context.getContentResolver().query(
                    uri,
                    new String[]{OpenableColumns.DISPLAY_NAME},
                    null,
                    null,
                    null);

            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(
                        OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    return cursor.getString(index);
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }

        return FILE_NAME;
    }
}
