package com.daniilvinogradov.dirt2;

import org.libsdl.app.SDLActivity;
import android.app.AlertDialog;
import android.text.InputType;
import android.view.WindowManager;
import android.widget.EditText;
import java.util.concurrent.CountDownLatch;

public final class Dirt2Activity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL3", "dirt2" };
    }
    // Called from the guest thread; Android's UI thread remains free to service
    // the dialog, keyboard and cancellation. Null means cancelled.
    public String promptText(String title, String message, String initial) {
        final CountDownLatch done = new CountDownLatch(1);
        final String[] result = new String[1];
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) { done.countDown(); return; }
            EditText input = new EditText(this);
            input.setSingleLine(true);
            input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS);
            input.setText(initial);
            input.setSelectAllOnFocus(true);
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title).setMessage(message).setView(input)
                .setPositiveButton(android.R.string.ok, (d,w) -> result[0] = input.getText().toString())
                .setNegativeButton(android.R.string.cancel, (d,w) -> {})
                .create();
            dialog.setOnDismissListener(d -> done.countDown());
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
            input.requestFocus();
        });
        try { done.await(); } catch (InterruptedException e) {
            Thread.currentThread().interrupt(); return null;
        }
        return result[0];
    }
}
