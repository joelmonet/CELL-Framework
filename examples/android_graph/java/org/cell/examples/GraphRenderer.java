// Original Cell Framework implementation; see LICENSE.
package org.cell.examples;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class GraphRenderer {
    static {
        System.loadLibrary("cell_android_graph");
    }

    private GraphRenderer() {}

    /** Call on a worker thread. The destination must be a new file. */
    public static void render(File project, File destination) throws IOException {
        byte[] error = renderNative(project.getAbsolutePath().getBytes(StandardCharsets.UTF_8),
                                    destination.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
        if (error != null) {
            throw new IOException(new String(error, StandardCharsets.UTF_8));
        }
    }

    private static native byte[] renderNative(byte[] project, byte[] destination);
}
