#pragma once
// Objects.hpp removed — migrated to LightningEntityManager

#include "../Audio/SoundManager.hpp"
#include "../Screenshot.hpp"

using namespace std;

auto ReadValue(string Data , int Start , int End){ // Reads Value from (Start to End) 
    string Out;
    for (int i = Start ; i <= End ; i ++){
        Out += Data[i]; 
    }
    return Out;
}


class TextSystem{
    public:
        bool Trigger = false;
        bool ReadLine = true;
        int LinePosition = 0;
        int FrameCounter = 0;
        bool AutoDismiss = false;
        int HoldFrames = 0;

        string Lines = "";
        string CurrentLine = "";
        string SecondLine = "";
        bool SEnable = false;
        Texture2D Bar;
        Font BarFont;
        // Typewriter blip: owned and played by Audio/SoundManager.hpp
        // (SoundManager::PlayTextNoise()), which also unloads it at shutdown.

        int ScaleValue = 0;
        Color TextColor = WHITE;

        int Red = 0;
        int Green = 0;
        int Blue = 0;

        Color TextBarColor = (Color){Red, Green, Blue, 255};

        // Log window state
        struct LogEntry {
            string text;
            int lifeFrames;
            Color color;
        };
        vector<LogEntry> logHistory;
        static constexpr int MAX_LOG_LINES = 8;
        static constexpr int LOG_LINE_HEIGHT = 22;
        static constexpr int LOG_WIDTH = 400;
        static constexpr int LOG_X = 10;
        static constexpr int LOG_Y_OFFSET = 10;  // from bottom

        void Update(){
            ScaleValue = int(GetScreenWidth() / 1280);
            TextBarColor = {Red, Green, Blue, Red};

            // Fade and age log entries
            for (auto it = logHistory.begin(); it != logHistory.end();) {
                it->lifeFrames--;
                if (it->lifeFrames <= 0) {
                    it = logHistory.erase(it);
                } else {
                    it->color.a = (unsigned char)(255 * min(1.0f, it->lifeFrames / 60.0f));
                    ++it;
                }
            }

// Draw log window background (bottom-left). Screenshot mode captures
            // the clean frame only, so the message log is skipped there (it is
            // HUD, and it was leaking "Collected ..." lines into captures).
            if (!g_shot.active && !logHistory.empty()) {
                int logH = min((int)logHistory.size(), MAX_LOG_LINES) * LOG_LINE_HEIGHT + 10;
                int logY = GetScreenHeight() - LOG_Y_OFFSET - logH;
                DrawRectangle(LOG_X - 5, logY - 5, LOG_WIDTH + 10, logH + 10,
                              Fade(BLACK, 0.7f));
                DrawRectangleLines(LOG_X - 5, logY - 5, LOG_WIDTH + 10, logH + 10,
                                   Fade(WHITE, 0.3f));
            }

            // Draw log entries (newest at bottom)
            int drawY = GetScreenHeight() - LOG_Y_OFFSET - LOG_LINE_HEIGHT;
            for (int i = (int)logHistory.size() - 1; i >= 0 && i >= (int)logHistory.size() - MAX_LOG_LINES; i--) {
                auto& e = logHistory[i];
                DrawTextEx(BarFont, e.text.c_str(),
                           { (float)LOG_X + 5, (float)drawY },
                           (float)LOG_LINE_HEIGHT - 2, 1, e.color);
                drawY -= LOG_LINE_HEIGHT;
            }

            if (Trigger){
                if (Red != 255){
                    Red ++;
                    Green ++;
                    Blue ++;
                }
                if (ReadLine){
                    for (int i = LinePosition ; i < (int)Lines.size(); i ++){
                        if (Lines[i] != '|'){
                            if (CurrentLine.size() != 51){
                                CurrentLine += Lines[i];
                            }
                            else {
                                SEnable = true;
                            }

                            if (SEnable){
                                SecondLine += Lines[i];
                            }

                            LinePosition ++;
                        }
                        else {
                            ReadLine = false;
                            break;
                        }
                        if (Lines[i] == '*'){
                            exit(0);
                        }
                        if (Lines[i] == ' '){
                            SoundManager::Instance().StopTextNoise();
                        }
                        if (Lines[i] == '/'){
                            // Commit current line(s) to log history
                            if (!CurrentLine.empty()) {
                                logHistory.push_back({CurrentLine, 300, WHITE});
                                if ((int)logHistory.size() > MAX_LOG_LINES * 2)
                                    logHistory.erase(logHistory.begin());
                            }
                            if (SEnable && !SecondLine.empty()) {
                                logHistory.push_back({SecondLine, 300, WHITE});
                                if ((int)logHistory.size() > MAX_LOG_LINES * 2)
                                    logHistory.erase(logHistory.begin());
                            }
                            CurrentLine = "";
                            SecondLine = "";
                            LinePosition = 0;
                            ReadLine = false;
                            Trigger = false;
                            FrameCounter = 0;
                            break;
                        }
                        if (Lines[i] == '*'){
                            exit(0);
                        }
                        if (Lines[i] == ' '){
                            SoundManager::Instance().StopTextNoise();
                        }
                    }
                    if (Trigger && ReadLine)
                        ReadLine = false;
                }
                else {
                    // Dialogue mode: typewriter effect for CurrentLine + SecondLine
                    DrawTextEx(BarFont, TextSubtext(TextFormat("%s" , CurrentLine.c_str() ), 0 , FrameCounter / 3 ) , { 190* ScaleValue, 720 / 2 - 100 + 80  } , 30 , 1, TextColor);

                    if (SEnable){
                        DrawTextEx(BarFont, TextSubtext(TextFormat("%s" , SecondLine.c_str() ), 0 , FrameCounter / 3) , { 190 * ScaleValue, 720 / 2 - 100 + 80  + 35 } , 30  , 1, TextColor);
                    }
                    int TotalChars = int((CurrentLine.size() + SecondLine.size()) * 3);
                    if (FrameCounter != TotalChars){
                        FrameCounter ++;
                    }

                    if (FrameCounter < TotalChars && FrameCounter % 14 == 1){
                        SoundManager::Instance().PlayTextNoise();
                    }

                    if (OmegaInputController.TextButton){
                        Red = 255;
                        Green = 255;
                        Blue = 255;
                        ReadLine = true;
                        LinePosition+=1;
                        CurrentLine = "";
                        SEnable = false;
                        SecondLine = "";
                        FrameCounter = 0;
                    }
                }
            }
            else {
                if (Red != 0){
                    Red -=5;
                    Green -=5;
                    Blue -=5;
                }
            }
        }

        void Write(string SayLines){
            Red = 0;
            Blue = 0;
            Green = 0;

            Lines = SayLines;
            CurrentLine = "";
            SecondLine = "";
            LinePosition = 0;
            ReadLine = true;
            Trigger = true;
            SEnable = false;
            FrameCounter = 0;
            AutoDismiss = SayLines.find('|') == string::npos &&
                          SayLines.find('/') == string::npos;

            // ALSO push to log window immediately (for both dialogue and toasts)
            logHistory.push_back({SayLines, 300, WHITE});
            if ((int)logHistory.size() > MAX_LOG_LINES * 2)
                logHistory.erase(logHistory.begin());
        }
};

static TextSystem AngelTechTextSystem;