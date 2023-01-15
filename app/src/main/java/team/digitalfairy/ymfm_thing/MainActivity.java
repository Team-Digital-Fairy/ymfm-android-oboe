package team.digitalfairy.ymfm_thing;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;

public class MainActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        System.loadLibrary("ymfm_thing");



    }

    @Override
    protected void onResume() {
        super.onResume();

        YmfmInterface.startOboe();
    }
}