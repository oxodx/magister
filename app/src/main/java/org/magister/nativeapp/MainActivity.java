package org.magister.nativeapp;

import org.libsdl.app.SDLActivity;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import org.json.JSONObject;
import android.app.Dialog;
import android.graphics.Color;
import android.view.ViewGroup;
import android.webkit.*;
import java.util.concurrent.CountDownLatch;

public final class MainActivity extends SDLActivity {
  private static final java.net.CookieManager COOKIES = new java.net.CookieManager(null, CookiePolicy.ACCEPT_ALL);
  static {
    CookieHandler.setDefault(COOKIES);
  }

  @Override
  protected String[] getLibraries() {
    return new String[] {"SDL2", "magister"};
  }

  /** Official web login; returns the configured OIDC callback URL. */
  public String webLogin(String startUrl) {
    final CountDownLatch done = new CountDownLatch(1);
    final String[] result = new String[] {""};
    runOnUiThread(() -> {
      Dialog dialog = new Dialog(this);
      WebView web = new WebView(this);
      web.setBackgroundColor(Color.WHITE);
      web.getSettings().setJavaScriptEnabled(true);
      web.getSettings().setDomStorageEnabled(true);
      web.setWebViewClient(new WebViewClient() {
        @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
          String url = request.getUrl().toString();
          if (url.contains("/oidc/redirect_callback.html")) {
            result[0] = url; dialog.dismiss(); done.countDown(); return true;
          }
          return false;
        }
      });
      dialog.setContentView(web, new ViewGroup.LayoutParams(-1, -1));
      dialog.setOnDismissListener(d -> done.countDown());
      dialog.show();
      if (dialog.getWindow() != null) {
        dialog.getWindow().setDimAmount(0.0f);
        dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.WHITE));
        dialog.getWindow().setLayout(-1, -1);
      }
      Map<String, String> language = new HashMap<>();
      language.put("Accept-Language", "en-US,en;q=0.9");
      web.loadUrl(startUrl, language);
    });
    try { done.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    return result[0];
  }

  /** Synchronous bridge used only by the C++ worker thread. */
  public String httpRequest(String method, String url, String body, String headersJson) {
    JSONObject out = new JSONObject();
    try {
      HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
      connection.setConnectTimeout(20000);
      connection.setReadTimeout(30000);
      connection.setInstanceFollowRedirects(true);
      connection.setRequestMethod(method);
      connection.setRequestProperty("User-Agent", "Mozilla/5.0 (Android) MagisterNative/1.0");
      connection.setRequestProperty("Accept", "application/json");
      connection.setRequestProperty("Accept-Language", "en-US,en;q=0.9");
      JSONObject headers = new JSONObject(headersJson == null ? "{}" : headersJson);
      for (Iterator<String> it = headers.keys(); it.hasNext();) {
        String key = it.next();
        connection.setRequestProperty(key, headers.getString(key));
      }
      if (body != null && !body.isEmpty()) {
        connection.setDoOutput(true);
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        connection.setFixedLengthStreamingMode(bytes.length);
        try (OutputStream stream = connection.getOutputStream()) {
          stream.write(bytes);
        }
      }
      int status = connection.getResponseCode();
      InputStream raw = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
      StringBuilder text = new StringBuilder();
      if (raw != null)
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(raw, StandardCharsets.UTF_8))) {
          for (String line; (line = reader.readLine()) != null;)
            text.append(line).append('\n');
        }
      out.put("status", status);
      out.put("url", connection.getURL().toString());
      out.put("body", text.toString());
      String xsrf = "";
      for (HttpCookie cookie : COOKIES.getCookieStore().getCookies())
        if (cookie.getName().equals("XSRF-TOKEN"))
          xsrf = cookie.getValue();
      out.put("xsrf", xsrf);
      connection.disconnect();
    } catch (Exception error) {
      try {
        out.put("error", error.getClass().getSimpleName() + ": " + error.getMessage());
      } catch (Exception ignored) {
      }
    }
    return out.toString();
  }
}
