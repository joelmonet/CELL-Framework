// Original Cell Framework implementation; see LICENSE.
package org.cell.examples;

import java.io.File;
import java.io.IOException;

public final class GraphRendererSmoke {
    private GraphRendererSmoke() {}

    public static void main(String[] args) throws IOException {
        if (args.length != 2)
            throw new IllegalArgumentException("Expected a graph project and a new output path");

        File project = new File(args[0]);
        File output = new File(args[1]);
        GraphRenderer.render(project, output);
        if (!output.isFile() || output.length() == 0)
            throw new IOException("JNI renderer did not produce a WAV file");

        try {
            GraphRenderer.render(project, output);
        } catch (IOException expected) {
            return;
        }
        throw new IOException("JNI renderer accepted an existing output");
    }
}
