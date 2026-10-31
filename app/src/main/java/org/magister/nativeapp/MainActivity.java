package org.magister.nativeapp;

import android.app.Dialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class MainActivity extends NativeActivity {
  private static final String SESSION_PREFS = "session";
  private static final String SCHOOL_KEY = "school";
  private static final String TOKEN_KEY = "access_token";
  private static final String KEY_ALIAS = "magister_session_key";

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
            result[0] = url;
            CookieManager.getInstance().flush();
            dialog.dismiss();
            done.countDown();
            return true;
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
    try {
      done.await();
    } catch (InterruptedException error) {
      Thread.currentThread().interrupt();
    }
    return result[0];
  }

  /** The school name is not sensitive and is kept for the next launch. */
  public String loadSchool() {
    return getSharedPreferences(SESSION_PREFS, MODE_PRIVATE)
        .getString(SCHOOL_KEY, "");
  }

  /** Decrypt the API token with an app-private Android Keystore key. */
  public String loadSessionToken() {
    String saved = getSharedPreferences(SESSION_PREFS, MODE_PRIVATE)
        .getString(TOKEN_KEY, "");
    if (saved == null || saved.isEmpty()) return "";
    try {
      String[] parts = saved.split(":", 2);
      if (parts.length != 2) return "";
      Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
      cipher.init(Cipher.DECRYPT_MODE, sessionKey(),
          new GCMParameterSpec(128, Base64.decode(parts[0], Base64.NO_WRAP)));
      return new String(cipher.doFinal(Base64.decode(parts[1], Base64.NO_WRAP)),
          StandardCharsets.UTF_8);
    } catch (Exception error) {
      getSharedPreferences(SESSION_PREFS, MODE_PRIVATE).edit().remove(TOKEN_KEY).apply();
      return "";
    }
  }

  /** Save the selected school and encrypt the API token at rest. */
  public void saveSession(String school, String accessToken) {
    try {
      Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
      cipher.init(Cipher.ENCRYPT_MODE, sessionKey());
      String encrypted = Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP) + ":"
          + Base64.encodeToString(
              cipher.doFinal(accessToken.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP);
      getSharedPreferences(SESSION_PREFS, MODE_PRIVATE).edit()
          .putString(SCHOOL_KEY, school)
          .putString(TOKEN_KEY, encrypted)
          .apply();
    } catch (Exception error) {
      throw new IllegalStateException("Unable to securely save the session", error);
    }
  }

  /** Remove both native credentials and the official login's browser cookies. */
  public void clearSession() {
    // Keep the harmless school slug so signing in again never requires typing
    // it. Only authentication state is removed.
    getSharedPreferences(SESSION_PREFS, MODE_PRIVATE).edit().remove(TOKEN_KEY).apply();
    runOnUiThread(() -> CookieManager.getInstance().removeAllCookies(
        ignored -> CookieManager.getInstance().flush()));
  }

  private SecretKey sessionKey() throws Exception {
    KeyStore store = KeyStore.getInstance("AndroidKeyStore");
    store.load(null);
    if (store.containsAlias(KEY_ALIAS))
      return ((KeyStore.SecretKeyEntry) store.getEntry(KEY_ALIAS, null)).getSecretKey();
    KeyGenerator generator = KeyGenerator.getInstance(
        KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
    generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
        KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
        .build());
    return generator.generateKey();
  }

  /** Open external content on the UI thread. */
  public void openUrl(String url) {
    runOnUiThread(() -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))));
  }

  @Override
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (event.getAction() == KeyEvent.ACTION_MULTIPLE) {
      String characters = event.getCharacters();
      if (characters != null && !characters.isEmpty()) {
        for (int index = 0; index < characters.length();) {
          int character = characters.codePointAt(index);
          nativeText(character);
          index += Character.charCount(character);
        }
        return true;
      }
    }
    if (event.getAction() == KeyEvent.ACTION_DOWN) {
      int meta = event.getMetaState();
      int printableMeta = KeyEvent.META_CTRL_ON | KeyEvent.META_ALT_ON | KeyEvent.META_META_ON;
      int character = event.getUnicodeChar(meta);
      if ((meta & printableMeta) == 0 && character >= 0x20 && character != 0x7F) {
        nativeText(character);
        return true;
      }
    }
    return super.dispatchKeyEvent(event);
  }

  private native void nativeText(int codepoint);
}
