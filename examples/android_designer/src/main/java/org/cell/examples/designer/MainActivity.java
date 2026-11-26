// Original Cell Framework implementation; see LICENSE.
package org.cell.examples.designer;

import android.app.Activity;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;

public final class MainActivity extends Activity {
    private static final Uri EDITOR_ORIGIN = Uri.parse("https://cell-editor.invalid");
    private static final String EDITOR_URL = EDITOR_ORIGIN + "/index.html";
    private static final int MAX_MESSAGE_BYTES = 64 * 1024;
    private static final int BACKGROUND = 0xff111b20;
    private static final int REFRESH_MS = 33;
    static { System.loadLibrary("cell_android_designer"); }

    private final Handler main = new Handler(Looper.getMainLooper());
    private LinearLayout layout;
    private Button toneButton;
    private TextView status;
    private WebView editor;
    private byte[] document;
    private WebMessagePort bridge;
    private long session;
    private boolean foreground;
    private boolean connected;
    private boolean playing;
    private boolean focusHeld;
    private boolean recoveredRenderer;
    private AudioManager audio;
    private AudioFocusRequest focus;

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (!foreground || !connected || session == 0) return;
            if (playing && !nativePlaying(session)) stopTone("Audio output ended");
            try {
                publish(nativeSnapshot(session, false));
                publish(nativeSnapshot(session, true));
            } catch (IllegalStateException error) {
                showEditorError(error.getMessage());
                return;
            }
            main.postDelayed(this, REFRESH_MS);
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        buildLayout();
        audio = getSystemService(AudioManager.class);
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setOnAudioFocusChangeListener(change -> {
                    if (change < 0 && focusHeld) stopTone("Audio focus changed");
                }, main).build();
        try {
            session = nativeCreate(state == null ? 0.0 : state.getDouble("gain"));
            document = nativeDocument();
            openEditor();
        } catch (IllegalStateException error) {
            showEditorError(error.getMessage());
        }
    }

    private void buildLayout() {
        layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(BACKGROUND);
        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(12), dp(4), dp(12), dp(4));
        toneButton = new Button(this);
        toneButton.setText("Play test tone");
        toneButton.setAllCaps(false);
        toneButton.setEnabled(false);
        toneButton.setOnClickListener(view -> {
            if (playing) stopTone("Tone off");
            else startTone();
        });
        toolbar.addView(toneButton, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(48)));
        status = new TextView(this);
        status.setText("Opening editor…");
        status.setTextColor(Color.WHITE);
        status.setPadding(dp(12), 0, 0, 0);
        status.setAccessibilityLiveRegion(TextView.ACCESSIBILITY_LIVE_REGION_POLITE);
        toolbar.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        layout.addView(toolbar);
        setContentView(layout);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            layout.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return insets;
            });
            layout.requestApplyInsets();
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void openEditor() {
        status.setText("Opening editor…");
        editor = new WebView(this);
        editor.setBackgroundColor(BACKGROUND);
        WebSettings settings = editor.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setBlockNetworkLoads(true);
        settings.setSupportMultipleWindows(false);
        settings.setJavaScriptCanOpenWindowsAutomatically(false);
        editor.setWebViewClient(new WebViewClient() {
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                if (request.isForMainFrame() && "GET".equals(request.getMethod())
                        && EDITOR_URL.equals(request.getUrl().toString())) {
                    return new WebResourceResponse("text/html", "UTF-8", 200, "OK",
                            Collections.singletonMap("Cache-Control", "no-store"), new ByteArrayInputStream(document));
                }
                return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found",
                        Collections.emptyMap(), new ByteArrayInputStream(new byte[0]));
            }

            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return true;
            }

            @Override public void onPageFinished(WebView view, String url) {
                if (view == editor && bridge == null && EDITOR_URL.equals(url)) connect(view);
            }

            @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (view == editor && request.isForMainFrame()) {
                    showEditorError("Could not open the editor");
                    destroyEditor();
                }
            }

            @Override public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
                if (view != editor) return true;
                showEditorError("Editor stopped");
                destroyEditor();
                if (!recoveredRenderer && !isFinishing()) {
                    recoveredRenderer = true;
                    openEditor();
                }
                return true;
            }
        });
        layout.addView(editor, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        editor.loadUrl(EDITOR_URL);
    }

    private void connect(WebView view) {
        try {
            WebMessagePort[] ends = view.createWebMessageChannel();
            bridge = ends[0];
            bridge.setWebMessageCallback(new WebMessagePort.WebMessageCallback() {
                @Override public void onMessage(WebMessagePort port, WebMessage message) {
                    if (port != bridge || session == 0) return;
                    String data = message.getData();
                    if (!connected && "cell-ready".equals(data)) {
                        connected = true;
                        toneButton.setEnabled(true);
                        status.setText("Tone off");
                        Log.i("CellDesigner", "Editor bridge connected");
                        resumeSnapshots();
                    } else if (connected && foreground && data != null && data.length() <= MAX_MESSAGE_BYTES) {
                        byte[] bytes = data.getBytes(StandardCharsets.UTF_8);
                        if (bytes.length > MAX_MESSAGE_BYTES) return;
                        try { nativeEdit(session, bytes); }
                        catch (IllegalStateException error) { showEditorError(error.getMessage()); }
                    }
                }
            }, main);
            view.postWebMessage(new WebMessage("cell-editor", new WebMessagePort[]{ends[1]}), EDITOR_ORIGIN);
        } catch (IllegalStateException error) {
            showEditorError("Could not connect the editor");
        }
    }

    private void publish(byte[] bytes) {
        if (bytes != null && bridge != null)
            bridge.postMessage(new WebMessage(new String(bytes, StandardCharsets.UTF_8)));
    }

    private void resumeSnapshots() {
        main.removeCallbacks(refresh);
        if (foreground && connected && session != 0) {
            nativeCancel(session);
            main.post(refresh);
        }
    }

    private void startTone() {
        if (!foreground || !connected || session == 0) return;
        if (audio == null || audio.requestAudioFocus(focus) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            status.setText("Audio focus unavailable");
            return;
        }
        focusHeld = true;
        try {
            nativeStart(session);
            playing = true;
            toneButton.setText("Stop test tone");
            status.setText("440 Hz test tone");
        } catch (IllegalStateException error) {
            stopTone(error.getMessage());
        }
    }

    private void stopTone(String message) {
        if (playing && session != 0) nativeStop(session);
        playing = false;
        if (focusHeld) {
            focusHeld = false;
            audio.abandonAudioFocusRequest(focus);
        }
        toneButton.setText("Play test tone");
        status.setText(message);
    }

    private void showEditorError(String message) {
        stopTone(message);
        connected = false;
        toneButton.setEnabled(false);
        main.removeCallbacks(refresh);
        if (session != 0) nativeCancel(session);
        if (bridge != null) {
            try { bridge.close(); }
            catch (IllegalStateException ignored) {}
            bridge = null;
        }
    }

    private void destroyEditor() {
        WebView view = editor;
        editor = null;
        if (view != null) {
            layout.removeView(view);
            view.destroy();
        }
    }

    @Override protected void onResume() {
        super.onResume();
        foreground = true;
        if (editor != null) editor.onResume();
        resumeSnapshots();
    }

    @Override protected void onPause() {
        foreground = false;
        main.removeCallbacks(refresh);
        if (playing) stopTone("Tone off");
        if (session != 0) nativeCancel(session);
        if (editor != null) {
            editor.evaluateJavascript("window.dispatchEvent(new Event('blur'));", null);
            editor.onPause();
        }
        super.onPause();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        if (session != 0) state.putDouble("gain", nativeGain(session));
    }

    @Override protected void onDestroy() {
        showEditorError("Tone off");
        destroyEditor();
        if (session != 0) nativeDestroy(session);
        session = 0;
        super.onDestroy();
    }

    private static native long nativeCreate(double gain);
    private static native void nativeDestroy(long handle);
    private static native byte[] nativeDocument();
    private static native void nativeStart(long handle);
    private static native void nativeStop(long handle);
    private static native boolean nativePlaying(long handle);
    private static native double nativeGain(long handle);
    private static native void nativeEdit(long handle, byte[] data);
    private static native void nativeCancel(long handle);
    private static native byte[] nativeSnapshot(long handle, boolean meters);
}
