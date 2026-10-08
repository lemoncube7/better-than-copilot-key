#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <stdexcept>
#include <new>
#include <cstddef>
#include "Touchpad.h"

static double clamp(double x, double lo, double hi) { return std::max(lo, std::min(hi, x)); }
static LARGE_INTEGER epoch, frequency;
static double now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - epoch.QuadPart) / frequency.QuadPart; }
static std::wstring directory, logPath, windowClass, snapshotPath;
static bool acceptTestInput = false;
static const UINT CopilotMessage = WM_APP + 23;
static const UINT RailWheelMessage = WM_APP + 24;
static const UINT RailPlaceMessage = WM_APP + 25;
static const UINT SettingsMessage = WM_APP + 26;
static LRESULT CALLBACK railMouseHook(int code,WPARAM message,LPARAM payload);
struct CopilotEvent { bool up; DWORD time; POINT position; int wheel=0;bool isWheel=false; };
struct HoldState {
    bool held = false;
    unsigned repeats = 0;
    bool update(bool up) {
        if(up) { held=false;repeats=0;return false; }
        if(held) { ++repeats;return false; }
        held=true;repeats=0;return true;
    }
};
static void log(const std::string& message) {
    if (logPath.empty()) return;
    HANDLE f = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(now() * 1000) + "ms " + message + "\r\n";
    DWORD count; WriteFile(f, line.data(), DWORD(line.size()), &count, NULL); CloseHandle(f);
}
static std::string readFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return "{}";
        throw std::runtime_error("Cannot read settings.json");
    }
    DWORD size = GetFileSize(f, NULL), count;
    if (size == INVALID_FILE_SIZE || size > 16384) { CloseHandle(f); throw std::runtime_error("Settings file is too large"); }
    std::string data(size, '\0'); BOOL ok = size == 0 || ReadFile(f, &data[0], size, &count, NULL);
    CloseHandle(f);
    if (!ok || (size && count != size)) throw std::runtime_error("Cannot read settings.json");
    if (data.size() >= 3 && BYTE(data[0]) == 0xEF && BYTE(data[1]) == 0xBB && BYTE(data[2]) == 0xBF) data.erase(0, 3);
    return data;
}
struct Json {
    const std::string& text; size_t at = 0;
    explicit Json(const std::string& s) : text(s) {}
    void space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at; }
    void expect(char c) { space(); if (at == text.size() || text[at++] != c) throw std::runtime_error("Invalid settings JSON"); }
    std::string quoted() {
        expect('"'); std::string out;
        while (at < text.size()) {
            unsigned char c = text[at++];
            if (c == '"') return out;
            if (c < 32) throw std::runtime_error("Invalid JSON string");
            if (c != '\\') { out += char(c); continue; }
            if (at == text.size()) break;
            char e = text[at++];
            if (e == '"' || e == '\\' || e == '/') out += e;
            else if (e == 'b') out += '\b'; else if (e == 'f') out += '\f';
            else if (e == 'n') out += '\n'; else if (e == 'r') out += '\r'; else if (e == 't') out += '\t';
            else if (e == 'u') {
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    if (at == text.size()) throw std::runtime_error("Invalid Unicode escape");
                    char h = text[at++]; int n = h >= '0' && h <= '9' ? h - '0' : h >= 'A' && h <= 'F' ? h - 'A' + 10 : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
                    if (n < 0) throw std::runtime_error("Invalid Unicode escape"); value = value * 16 + n;
                }
                if (value < 128) out += char(value);
                else if (value < 2048) { out += char(0xC0 | (value >> 6)); out += char(0x80 | (value & 63)); }
                else { out += char(0xE0 | (value >> 12)); out += char(0x80 | ((value >> 6) & 63)); out += char(0x80 | (value & 63)); }
            } else throw std::runtime_error("Invalid JSON escape");
        }
        throw std::runtime_error("Unterminated JSON string");
    }
    std::map<std::string, std::string> parse() {
        std::map<std::string, std::string> values; expect('{'); space();
        if (at < text.size() && text[at] == '}') ++at;
        else while (true) {
            std::string key = quoted(); expect(':'); space();
            if (at < text.size() && text[at] == '"') values[key] = quoted();
            else {
                size_t start = at;
                while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ' ' && text[at] != '\r' && text[at] != '\n' && text[at] != '\t') ++at;
                std::string token = text.substr(start, at - start); char* end = NULL;
                double number = std::strtod(token.c_str(), &end);
                if (token != "true" && token != "false" && (token.empty() || *end || !std::isfinite(number))) throw std::runtime_error("Invalid JSON number");
                values[key] = token;
            }
            space(); if (at == text.size()) throw std::runtime_error("Unterminated JSON object");
            char c = text[at++]; if (c == '}') break; if (c != ',') throw std::runtime_error("Invalid JSON object");
        }
        space(); if (at != text.size()) throw std::runtime_error("Trailing JSON data"); return values;
    }
};
struct Settings {
    std::string drawingSurface="screen";
    std::string effect = "cluster", palette = "neon", fire = "instant";
    int confettiCount = 280, gravityCount = 130, clusterCount = 150;
    double confettiLife = 3.4, confettiSpeed = 1, gravityLife = 11, strength = 1, radius = 16, fuse = .9, speed = 1, slingPower = 6;
    double gravity = 600; bool gravityEnabled = false;
    int blackHoleCount=180,blackHoleFeed=15;
    double blackHoleStrength=1,pendulumLength=180,pendulumGravity=1000,pendulumDamping=.08;
    double blackHoleGrowth=.08,blackHoleOrbitDamping=.01,blackHoleOrbitRatio=.8;
    double crossRate=120,crossReach=450;int crossBurst=300;
    bool pendulumElastic=false;double pendulumStiffness=60,pendulumMaxStretch=3;
    double railCharge=8,railPower=1;int railImpact=200;
    double diceSize=76,diceSensitivity=1,diceStopDelay=.18;
    static Settings from(const std::string& json) {
        auto values = Json(json).parse(); Settings s;
        auto str = [&](const char* key, std::string& target) { auto i = values.find(key); if (i != values.end()) target = i->second; };
        auto num = [&](const char* key, double fallback, double lo, double hi) {
            auto i = values.find(key); if (i == values.end()) return fallback;
            char* end = NULL; double v = std::strtod(i->second.c_str(), &end);
            if (end == i->second.c_str() || *end || !std::isfinite(v)) throw std::runtime_error("Invalid setting value");
            return clamp(v, lo, hi);
        };
        str("DrawingSurface",s.drawingSurface);if(s.drawingSurface!="white")s.drawingSurface="screen";
        str("Effect", s.effect); str("Palette", s.palette); str("FireMode", s.fire);
        if (s.effect != "cluster" && s.effect != "gravity" && s.effect != "confetti" && s.effect != "blackhole" && s.effect != "pendulum" && s.effect != "crossflash" && s.effect != "railgun" && s.effect != "dice" && s.effect != "touchpad" && s.effect != "drawing" && s.effect != "homing") s.effect = "cluster";
        if (s.palette != "neon" && s.palette != "pastel" && s.palette != "ice" && s.palette != "warm") s.palette = "neon";
        if (s.fire != "instant" && s.fire != "left" && s.fire != "middle" && s.fire != "space" && s.fire != "slingshot") s.fire = "instant";
        s.confettiCount = int(num("ConfettiCount", 280, 20, 1200)); s.gravityCount = int(num("GravityCount", 130, 10, 800)); s.clusterCount = int(num("ClusterCount", 150, 20, 1000));
        s.confettiLife = num("ConfettiLife", 3.4, .5, 10); s.confettiSpeed = num("ConfettiSpeed", 1, .2, 3);
        s.gravityLife = num("GravityLife", 11, 1, 30); s.strength = num("GravityStrength", 1, .1, 5); s.radius = num("CollisionRadius", 16, 4, 60);
        s.fuse = num("ClusterFuse", .9, .1, 5); s.speed = num("ClusterSpeed", 1, .1, 4); s.slingPower=num("SlingshotPower",6,1,15);
        s.gravity=num("ClusterGravity",600,50,3000);
        auto enabled=values.find("ClusterGravityEnabled");if(enabled!=values.end())s.gravityEnabled=enabled->second=="true";
        s.blackHoleCount=int(num("BlackHoleCount",180,30,800));s.blackHoleStrength=num("BlackHoleStrength",1,.1,5);
        s.blackHoleFeed=int(num("BlackHoleFeedCount",std::max(6,s.blackHoleCount/12),0,200));
        s.blackHoleGrowth=num("BlackHoleGrowth",.08,.01,1);s.blackHoleOrbitDamping=num("BlackHoleOrbitDamping",.01,0,.3);s.blackHoleOrbitRatio=num("BlackHoleOrbitRatio",.8,0,1);
        s.pendulumLength=num("PendulumLength",180,80,360);s.pendulumGravity=num("PendulumGravity",1000,200,3000);s.pendulumDamping=num("PendulumDamping",.08,0,1);
        s.crossRate=num("CrossGatherRate",120,20,400);s.crossReach=num("CrossReach",450,100,900);s.crossBurst=int(num("CrossBurstCount",300,50,1200));
        auto elastic=values.find("PendulumElasticEnabled");if(elastic!=values.end())s.pendulumElastic=elastic->second=="true";
        s.pendulumStiffness=num("PendulumStiffness",60,10,300);s.pendulumMaxStretch=num("PendulumMaxStretch",3,1.2,5);
        s.railCharge=num("RailChargePerNotch",8,1,25);s.railPower=num("RailPower",1,.5,2);s.railImpact=int(num("RailImpactCount",200,50,800));s.diceSize=num("DiceSize",76,40,140);s.diceSensitivity=num("DiceSensitivity",1,.3,3);s.diceStopDelay=num("DiceStopDelay",.18,.08,.6);return s;
    }
};
struct Bounds { double left, top, right, bottom; };
struct Particle {
    int kind = 0; double x = 0, y = 0, vx = 0, vy = 0, size = 3, angle = 0, spin = 0, born = 0, life = 1, strength = 1, radius = 16; COLORREF color = RGB(255,255,255);
    int railStyle=0;bool confined = false; Bounds bounds = {}; double fall=70,drag=1.6,travel=0;int arm=0;
};
struct Cluster {
    double x, y, vx, vy, born; Bounds bounds; Settings settings; COLORREF color;
    bool trajectoryReady = false; double originX = 0, originY = 0, edgeTime = INFINITY, edgeX = 0, edgeY = 0; unsigned edgeSides = 0;
    bool railSlug=false;double railEnergy=0;
};
struct Pending { double x, y, due; Settings settings; };
struct SlingState {
    bool ready=false,pulling=false;
    double anchorX=0,anchorY=0,pouchX=0,pouchY=0,until=0;
    Bounds bounds={}; Settings settings;
    void pull(double cx,double cy) {
        double dx=cx-anchorX,dy=cy-anchorY,length=std::hypot(dx,dy);
        double scale=length>240?240/length:1;
        pouchX=clamp(anchorX+dx*scale,bounds.left,bounds.right);
        pouchY=clamp(anchorY+dy*scale,bounds.top,bounds.bottom);
    }
    double vx()const{return (anchorX-pouchX)*settings.slingPower;}
    double vy()const{return (anchorY-pouchY)*settings.slingPower;}
};
struct PendulumState {
    bool ready=false,hanging=false;
    double anchorX=0,anchorY=0,length=180,angle=0,omega=0,last=0,until=0;
    double anchorVX=0,anchorVY=0;
    double restLength=180,radialSpeed=0;
    Settings settings;
    double x()const{return anchorX+length*std::sin(angle);}
    double y()const{return anchorY+length*std::cos(angle);}
    double vx()const{return anchorVX+radialSpeed*std::sin(angle)+length*omega*std::cos(angle);}
    double vy()const{return anchorVY+radialSpeed*std::cos(angle)-length*omega*std::sin(angle);}
    void step(double dt,double ax=0,double ay=0) {
        dt=clamp(dt,0,.25);
        while(dt>1e-9) {
            double h=std::min(dt,1./240.);
            if(settings.pendulumElastic) {
                double radialAcceleration=length*omega*omega+(settings.pendulumGravity-ay)*std::cos(angle)-ax*std::sin(angle)-settings.pendulumStiffness*(length-restLength)-1.6*std::sqrt(settings.pendulumStiffness)*radialSpeed;
                radialSpeed=clamp(radialSpeed+radialAcceleration*h,-6000,6000);
                length+=radialSpeed*h;
                if(length<restLength){length=restLength;radialSpeed=std::max(0.,radialSpeed);}
                if(length>restLength*settings.pendulumMaxStretch){length=restLength*settings.pendulumMaxStretch;radialSpeed=std::min(0.,radialSpeed);}
            } else radialSpeed=0;
            omega+=(-(settings.pendulumGravity-ay)/length*std::sin(angle)-ax/length*std::cos(angle)-settings.pendulumDamping*omega-(settings.pendulumElastic?2*radialSpeed*omega/length:0))*h;
            omega=clamp(omega,-30,30);
            angle+=omega*h;dt-=h;
        }
    }
    void follow(double x,double y,double dt) {
        if(dt>.001) {
            double rawVX=clamp((x-anchorX)/dt,-12000,12000),rawVY=clamp((y-anchorY)/dt,-12000,12000);
            double blend=1-std::exp(-dt/.045),nextVX=anchorVX+(rawVX-anchorVX)*blend,nextVY=anchorVY+(rawVY-anchorVY)*blend;
            step(dt,clamp((nextVX-anchorVX)/dt,-120000,120000),clamp((nextVY-anchorVY)/dt,-120000,120000));
            anchorVX=nextVX;anchorVY=nextVY;
        }
        anchorX=x;anchorY=y;
    }
};
struct BlackHoleState {
    bool ready=false,pulling=false;
    double x=0,y=0,until=0,lastFeed=0,initialRemaining=0,spawnCredit=0;int captured=0;Settings settings;
    std::vector<Bounds> spawnCells;size_t spawnCell=0;
    // dr/dN = GrowthFourthPerStar / (4*r^3). Match the previous radius at N=400.
    static constexpr double GrowthFourthPerStar = 2250257.4201348354;
    double radius()const{return std::min(320.,std::pow(65536.+GrowthFourthPerStar*settings.blackHoleGrowth*std::max(0,captured),.25));}
    double mu()const{return 24000000*settings.blackHoleStrength*(1+std::max(0,captured)*.0015);}
    double softening()const{return std::max(8.,radius()*.12);}
    double acceleration(double distance)const {
        double softened=distance*distance+softening()*softening();
        return mu()*distance/(softened*std::sqrt(softened));
    }
    double circularSpeed(double distance)const{return std::sqrt(distance*acceleration(distance));}
};
struct Shock {double x,y,born;COLORREF color;};
struct CollectedBurst {int remaining;double x,y;Bounds bounds;Settings settings;double radius=16,jetAngle=0;};
struct CrossState {
    bool ready=false,pulling=false;double x=0,y=0,until=0,credit=0,flashAt=-100;int collected=0,arm=0;Settings settings;
    double charge()const{return clamp(collected/96.,0,1);}
};
struct RailState {
    bool ready=false,holding=false;double x=0,y=0,dx=1,dy=0,energy=0,until=0,visibleUntil=-100,firedAt=-100;Settings settings;
    void aim(double cx,double cy){double length=std::hypot(cx-x,cy-y);if(length>20){dx=(cx-x)/length;dy=(cy-y)/length;}}
    void charge(int delta){energy=clamp(energy+std::abs(double(delta))/120*settings.railCharge,0,100);}
    POINT world(double along,double side,double recoil=0)const{return {LONG(x+dx*(along*1.25-recoil)-dy*side*1.25),LONG(y+dy*(along*1.25-recoil)+dx*side*1.25)};}
};
struct DiceState {
    bool visible=false,holding=false,settled=true;
    double x=0,y=0,lastX=0,lastY=0,lastMotion=0,until=0;
    double angle[3]={0,0,0},velocity[3]={0,0,0};Settings settings;
    void step(double cx,double cy,double dt,double time) {
        double dx=cx-lastX,dy=cy-lastY;lastX=cx;lastY=cy;
        if(holding){x=cx+95;y=cy-65;}
        if(holding&&std::hypot(dx,dy)>1){lastMotion=time;settled=false;
            velocity[0]=clamp(velocity[0]+dy*.055*settings.diceSensitivity,-30,30);
            velocity[1]=clamp(velocity[1]+dx*.055*settings.diceSensitivity,-30,30);
            velocity[2]=clamp(velocity[2]+(dx-dy)*.018*settings.diceSensitivity,-20,20);
        }
        bool stopping=!holding||time-lastMotion>settings.diceStopDelay;
        double error=0;
        for(int i=0;i<3;++i){
            if(stopping){velocity[i]=0;double target=std::round(angle[i]/1.5707963267948966)*1.5707963267948966;
                angle[i]+=(target-angle[i])*(1-std::exp(-dt*18));error+=std::abs(target-angle[i]);
            }else{angle[i]+=velocity[i]*dt;velocity[i]*=std::exp(-dt*2);}
            angle[i]=std::remainder(angle[i],6.283185307179586);
        }
        settled=stopping&&error<.012;
        if(!holding&&time>=until)visible=false;
    }
};
struct RailTrace {double x,y,endX,endY,born,energy;COLORREF color;};
static void trajectory(Cluster& c) {
    c.originX = clamp(c.x,c.bounds.left,c.bounds.right);c.originY = clamp(c.y,c.bounds.top,c.bounds.bottom);
    double tx = INFINITY, ty = INFINITY;
    if(c.vx>0)tx=(c.bounds.right-c.originX)/c.vx;else if(c.vx<0)tx=(c.bounds.left-c.originX)/c.vx;
    unsigned ySide=0;
    double g=c.settings.gravityEnabled?c.settings.gravity:0;
    if(g==0) {
        if(c.vy>0){ty=(c.bounds.bottom-c.originY)/c.vy;ySide=8;}
        else if(c.vy<0){ty=(c.bounds.top-c.originY)/c.vy;ySide=4;}
    } else {
        // Solve y0 + vy*t + g*t*t/2 = boundary, using stable quadratic roots.
        auto consider=[&](double boundary,unsigned side) {
            double a=g*.5,b=c.vy,d=c.originY-boundary,disc=b*b-4*a*d;
            if(disc<0)return;
            double q=-.5*(b+std::copysign(std::sqrt(std::max(0.,disc)),b));
            double roots[2]={q/a,q==0?0:d/q};
            for(double t:roots) {
                if(t<0||!std::isfinite(t))continue;
                double velocity=c.vy+g*t;
                bool outward=side==4?velocity<=1e-7:velocity>=-1e-7;
                if(t<1e-12)outward=side==4?c.vy<0:c.vy>=0;
                if(outward&&t<ty){ty=t;ySide=side;}
            }
        };
        consider(c.bounds.top,4);consider(c.bounds.bottom,8);
    }
    c.edgeTime=std::min(tx,ty);c.edgeSides=0;
    if(std::isfinite(c.edgeTime)) {
        if(std::abs(tx-c.edgeTime)<1e-9)c.edgeSides|=c.vx>0?2:1;
        if(std::abs(ty-c.edgeTime)<1e-9)c.edgeSides|=ySide;
        c.edgeX=clamp(c.originX+c.vx*c.edgeTime,c.bounds.left,c.bounds.right);
        c.edgeY=clamp(c.originY+c.vy*c.edgeTime+.5*g*c.edgeTime*c.edgeTime,c.bounds.top,c.bounds.bottom);
        if(c.edgeSides&1)c.edgeX=c.bounds.left;if(c.edgeSides&2)c.edgeX=c.bounds.right;
        if(c.edgeSides&4)c.edgeY=c.bounds.top;if(c.edgeSides&8)c.edgeY=c.bounds.bottom;
    }
    c.trajectoryReady=true;
}
static bool move(Cluster& c, double dt) {
    const Bounds& b = c.bounds; c.x = clamp(c.x, b.left, b.right); c.y = clamp(c.y, b.top, b.bottom);
    if (c.x <= b.left || c.x >= b.right || c.y <= b.top || c.y >= b.bottom) return true;
    double dx = c.vx * dt, dy = c.vy * dt, fraction = 1; bool hit = false;
    if (dx > 0 && c.x + dx >= b.right) { fraction = std::min(fraction, (b.right - c.x) / dx); hit = true; }
    if (dx < 0 && c.x + dx <= b.left) { fraction = std::min(fraction, (b.left - c.x) / dx); hit = true; }
    if (dy > 0 && c.y + dy >= b.bottom) { fraction = std::min(fraction, (b.bottom - c.y) / dy); hit = true; }
    if (dy < 0 && c.y + dy <= b.top) { fraction = std::min(fraction, (b.top - c.y) / dy); hit = true; }
    c.x += dx * fraction; c.y += dy * fraction; return hit;
}
static bool collision(double x, double y, double nx, double ny, double cx, double cy, double radius, double& hx, double& hy) {
    double dx = nx - x, dy = ny - y, length = dx*dx + dy*dy;
    double t = length < .001 ? 0 : clamp(((cx-x)*dx + (cy-y)*dy)/length, 0, 1);
    hx = x + t*dx; hy = y + t*dy;
    return (hx-cx)*(hx-cx) + (hy-cy)*(hy-cy) <= radius*radius;
}
static bool down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
static COLORREF fadeColor(COLORREF c, double fade) { return RGB(std::max(1, int(GetRValue(c)*fade)), std::max(1, int(GetGValue(c)*fade)), std::max(1, int(GetBValue(c)*fade))); }
static std::mt19937 generator(GetTickCount());
static double random01() { return std::generate_canonical<double, 32>(generator); }
static COLORREF colorFor(const std::string& palette) {
    static const COLORREF palettes[4][5] = {
        { RGB(255,83,125), RGB(255,207,64), RGB(72,217,255), RGB(156,111,255), RGB(100,238,153) },
        { RGB(255,180,200), RGB(190,185,255), RGB(180,240,215), RGB(255,230,170), RGB(180,215,255) },
        { RGB(100,210,255), RGB(185,245,255), RGB(130,150,255), RGB(255,255,255), RGB(160,220,255) },
        { RGB(255,90,65), RGB(255,160,45), RGB(255,215,85), RGB(255,120,160), RGB(255,200,120) }
    };
    int index = palette == "pastel" ? 1 : palette == "ice" ? 2 : palette == "warm" ? 3 : 0;
    return palettes[index][generator()%5];
}
static COLORREF colorForFixed(const std::string& palette) {
    return palette=="warm"?RGB(255,200,95):palette=="ice"?RGB(150,225,255):palette=="pastel"?RGB(210,185,255):RGB(130,205,255);
}
struct Engine {
    HWND window = NULL; BYTE* bufferPixels = NULL; BYTE* coveragePixels = NULL; HDC coverageDC=NULL; HBITMAP coverageBitmap=NULL; HGDIOBJ coverageOld=NULL; HDC buffer = NULL; HBITMAP bitmap = NULL; HGDIOBJ oldBitmap = NULL; HFONT font = NULL;
    int left, top, width, height, triggers = 0, explosions = 0, collisions = 0;
    std::vector<Particle> particles; std::vector<Cluster> clusters; std::vector<Pending> pending;
    Settings armed; bool isArmed = false, leftDown = false, middleDown = false, spaceDown = false, firstPaint = false;
    double started = 0, previous = 0, armedUntil = 0, mouseVX = 0, mouseVY = 0; POINT previousCursor = {};
    RECT previousBounds = {}, pendingPaint = {}; bool closing = false, snapshotSaved = false;
    HHOOK keyboardHook = NULL;
    HHOOK mouseHook = NULL;bool railInputHeld=false;
    HoldState copilotHold;
    double holdStarted = 0;
    std::vector<CopilotEvent> startupEvents;
    SlingState sling;
    PendulumState pendulum;
    BlackHoleState blackHole;
    std::vector<Shock> shocks;
    std::vector<CollectedBurst> collectedBursts;
    CrossState cross;
    RailState rail;std::vector<RailState> extraRails;
    struct GuidedShot {double x,y,vx,vy,targetX,targetY,goalX,goalY,nx,ny,born;Bounds bounds;COLORREF color;};
    std::vector<GuidedShot> guidedShots;
    bool homingReady=false,homingHolding=false;double homingX=0,homingY=0,homingUntil=0;
    Settings homingSettings;
    void releaseHoming(double tx,double ty,double time){
        if(!homingReady||!homingHolding)return;
        homingReady=false;homingHolding=false;
        Bounds b=monitor(homingX,homingY);b.left+=8;b.top+=8;b.right-=8;b.bottom-=8;
        tx=clamp(tx,b.left,b.right);ty=clamp(ty,b.top,b.bottom);
        double dx=tx-homingX,dy=ty-homingY;
        if(std::hypot(dx,dy)<40){tx=clamp(homingX+(homingX<(b.left+b.right)/2?320:-320),b.left,b.right);dx=tx-homingX;}
        double distance=std::max(1.,std::hypot(dx,dy)),angle=std::atan2(dy,dx);
        for(int group=0;group<10;++group)for(int j=0;j<3;++j){
            double a=angle+3.14159265359+(random01()-.5)*.78539816339,speed=960+random01()*120;
            guidedShots.push_back({homingX,homingY,std::cos(a)*speed,std::sin(a)*speed,tx,ty,
                homingX+dx*.66,homingY+dy*.66,dx/distance,dy/distance,time+group/30.,b,colorFor(homingSettings.palette)});
        }
        log("Homing=Volley Count=30 Target="+std::to_string(tx)+","+std::to_string(ty));invalidate();
    }
    void stepHoming(double time,double dt){
        for(size_t i=guidedShots.size();i-- >0;){
            GuidedShot& m=guidedShots[i];if(time<m.born)continue;
            double remaining=std::min(dt,time-m.born);bool hit=false;
            while(remaining>0){
                double h=std::min(remaining,1./240.),dx=m.goalX-m.x,dy=m.goalY-m.y,d=std::max(1.,std::hypot(dx,dy));
                m.vx+=dx/d*1800*h;m.vy+=dy/d*1800*h;
                double ox=m.x,oy=m.y,old=(ox-m.targetX)*m.nx+(oy-m.targetY)*m.ny;
                m.x+=m.vx*h;m.y+=m.vy*h;
                double next=(m.x-m.targetX)*m.nx+(m.y-m.targetY)*m.ny;
                if(old<0&&next>=0){double t=-old/(next-old);m.x=ox+(m.x-ox)*t;m.y=oy+(m.y-oy)*t;hit=true;break;}
                remaining-=h;
            }
            if(hit||time-m.born>6){
                m.x=clamp(m.x,m.bounds.left,m.bounds.right);m.y=clamp(m.y,m.bounds.top,m.bounds.bottom);
                shocks.push_back({m.x,m.y,time,m.color});++explosions;
                guidedShots[i]=guidedShots.back();guidedShots.pop_back();
            }
        }
    }
    void drawHoming(double time){
        if(homingReady){star(homingX,homingY,10,RGB(255,255,255));
            const wchar_t* text=L"커서를 목표로 옮기고 놓으면 연속 발사";SetTextColor(buffer,RGB(180,220,255));TextOutW(buffer,int(homingX+20),int(homingY+20),text,lstrlenW(text));}
        for(const GuidedShot& m:guidedShots)if(time>=m.born){
            double speed=std::max(1.,std::hypot(m.vx,m.vy));
            line(m.x-m.vx/speed*16,m.y-m.vy/speed*16,m.x,m.y,fadeColor(m.color,.45));
            star(m.x,m.y,3,m.color);star(m.x,m.y,1,RGB(255,255,255));
        }
    }
    DiceState dice;bool middleCaptured=false;
    std::vector<RailTrace> railTraces;
    bool cancelUntilUp=false,settingsPending=false,f12Captured=false;
    #include "TouchpadDraw.inl"
    #include "TouchpadEngine.inl"
    Engine() {
        left = GetSystemMetrics(SM_XVIRTUALSCREEN); top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN); height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        particles.reserve(4096);
    }
    ~Engine() {
        freeInk();
        if(coverageDC){SelectObject(coverageDC,coverageOld);DeleteDC(coverageDC);}
        if(coverageBitmap)DeleteObject(coverageBitmap);
        if(keyboardHook)UnhookWindowsHookEx(keyboardHook);
        if(mouseHook)UnhookWindowsHookEx(mouseHook);
        if (buffer) { SelectObject(buffer, oldBitmap); DeleteDC(buffer); }
        if (bitmap) DeleteObject(bitmap); if (font) DeleteObject(font);
    }
    void setupBuffer() {
        HDC screen = GetDC(NULL); buffer = CreateCompatibleDC(screen); BITMAPINFO info={};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap = CreateDIBSection(screen,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&bufferPixels),NULL,0); coverageDC=CreateCompatibleDC(screen);
        coverageBitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&coveragePixels),NULL,0);
        ReleaseDC(NULL, screen);
        if(!coverageDC||!coverageBitmap)throw std::runtime_error("Cannot create opacity buffer");
        coverageOld=SelectObject(coverageDC,coverageBitmap);
        SelectObject(coverageDC,GetStockObject(DC_PEN));SelectObject(coverageDC,GetStockObject(DC_BRUSH));
        RECT coverageRect={0,0,width,height};FillRect(coverageDC,&coverageRect,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if (!buffer || !bitmap) throw std::runtime_error("Cannot create effect buffer");
        oldBitmap = SelectObject(buffer, bitmap); RECT all = {0,0,width,height}; FillRect(buffer,&all,(HBRUSH)GetStockObject(BLACK_BRUSH));
        SelectObject(buffer, GetStockObject(DC_PEN)); SelectObject(buffer, GetStockObject(DC_BRUSH));
        font = CreateFontW(18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"맑은 고딕");
        SelectObject(buffer,font); SetBkMode(buffer,TRANSPARENT);
    }
    Bounds monitor(double x, double y) {
        POINT pt = {LONG(x+left),LONG(y+top)}; MONITORINFO info = {}; info.cbSize=sizeof(info);
        GetMonitorInfoW(MonitorFromPoint(pt,MONITOR_DEFAULTTONEAREST),&info);
        return {double(info.rcMonitor.left-left),double(info.rcMonitor.top-top),double(info.rcMonitor.right-left),double(info.rcMonitor.bottom-top)};
    }
    static BOOL CALLBACK monitors(HMONITOR m,HDC,LPRECT,LPARAM param) {
        auto output = reinterpret_cast<std::vector<RECT>*>(param); MONITORINFO i={}; i.cbSize=sizeof(i); GetMonitorInfoW(m,&i); output->push_back(i.rcMonitor); return TRUE;
    }
    void launch(const Settings& s,double x,double y,double vx,double vy,double time) {
        Bounds b=monitor(x,y); b.left+=13;b.top+=13;b.right-=13;b.bottom-=13;
        clusters.push_back({x,y,vx*s.speed,vy*s.speed,time,b,s,colorFor(s.palette)});
        trajectory(clusters.back());
        if(clusters.size()>80) clusters.erase(clusters.begin());
        log("LaunchVelocity="+std::to_string(vx*s.speed)+","+std::to_string(vy*s.speed));
    }
    void trigger(const Settings& s,const POINT* position=NULL) {
        if(cancelUntilUp)return;
        padReady=false;padRail=false;railPadHeads.clear();railPadCredit=0;padContacts.clear();drawing=false;penDown=false;
        double time=now(); POINT cursor;if(position)cursor=*position;else GetCursorPos(&cursor);isArmed=false;sling.ready=false;sling.pulling=false;
        pendulum.ready=false;pendulum.hanging=false;blackHole.ready=false;blackHole.pulling=false;
        cross.ready=false;cross.pulling=false;rail.ready=false;rail.holding=false;extraRails.clear();dice.visible=false;homingReady=false;homingHolding=false;
        for(Particle& p:particles)if(p.kind==3||p.kind==4){p.kind=2;p.born=time;p.life=1.2;}
        log("Trigger="+std::to_string(++triggers)+" Effect="+s.effect+" Window="+std::to_string(reinterpret_cast<UINT_PTR>(window)));
        if(s.effect=="touchpad"||s.effect=="drawing") {
            padSettings=s;padBounds=monitor(cursor.x-left,cursor.y-top);padUntil=time+12;padLastReport=-100;padPackets=0;padEmission=0;
            RAWINPUTDEVICE device={0x0D,5,RIDEV_INPUTSINK,window};padReady=RegisterRawInputDevices(&device,1,sizeof(device))!=FALSE;
            log(padReady?"Touchpad=Registered":"Touchpad=RegistrationFailed");
            if(s.effect=="drawing"){if(!padReady)throw std::runtime_error("Cannot register drawing input");openBoard(padBounds);}
        } else if(s.effect=="confetti") {
            std::vector<RECT> displays;EnumDisplayMonitors(NULL,NULL,monitors,reinterpret_cast<LPARAM>(&displays));
            for(const RECT& r:displays) for(int i=0;i<s.confettiCount;++i) {
                bool right=i%2!=0;double angle=-1.57079632679+(random01()-.5)*1.8+(right?-.25:.25),speed=(450+random01()*650)*s.confettiSpeed;
                Particle p;p.x=r.left-left+(r.right-r.left)*(right?.78:.22);p.y=r.top-top+(r.bottom-r.top)*.76;
                p.vx=std::cos(angle)*speed;p.vy=std::sin(angle)*speed;p.size=5+random01()*7;p.angle=random01()*360;p.spin=(random01()-.5)*700;p.born=time;p.life=s.confettiLife;p.color=colorFor(s.palette);particles.push_back(p);
            }
        } else if(s.effect=="gravity") {
            double cx=cursor.x-left,cy=cursor.y-top;Bounds b=monitor(cx,cy);
            for(int i=0;i<s.gravityCount;++i) {
                double angle=random01()*6.28318530718,radius=130+random01()*380;
                Particle p;p.kind=1;p.x=clamp(cx+std::cos(angle)*radius,b.left+8,b.right-8);p.y=clamp(cy+std::sin(angle)*radius,b.top+8,b.bottom-8);
                double dx=p.x-cx,dy=p.y-cy,distance=std::max(1.,std::hypot(dx,dy)),speed=45+random01()*75;
                p.vx=-dy/distance*speed;p.vy=dx/distance*speed;p.size=2+random01()*2;p.born=time;p.life=s.gravityLife*(.8+.2*random01());p.strength=s.strength;p.radius=s.radius;p.color=colorFor(s.palette);particles.push_back(p);
            }
        } else if(s.effect=="dice") {
            dice=DiceState();dice.settings=s;dice.visible=true;dice.holding=copilotHold.held;
            dice.lastX=cursor.x-left;dice.lastY=cursor.y-top;dice.x=dice.lastX+95;dice.y=dice.lastY-65;
            dice.lastMotion=time;dice.until=time+8;log("Dice=Ready");
        } else if(s.effect=="homing") {
            homingSettings=s;homingX=cursor.x-left;homingY=cursor.y-top;homingReady=true;homingHolding=true;homingUntil=time+8;
            if(!copilotHold.held)releaseHoming(homingX,homingY,time);
        } else if(s.effect=="railgun") {
            rail.settings=s;rail.x=cursor.x-left;rail.y=cursor.y-top;rail.dx=1;rail.dy=0;rail.energy=0;
            rail.ready=true;rail.holding=copilotHold.held;rail.until=time+8;rail.visibleUntil=-100;rail.firedAt=-100;
            RAWINPUTDEVICE device={0x0D,5,RIDEV_INPUTSINK,window};padRail=true;padReady=RegisterRawInputDevices(&device,1,sizeof(device))!=FALSE;padLastReport=-100;padPackets=0;log(padReady?"RailTouchpad=Registered":"RailTouchpad=Unavailable");
            log(std::string("Rail=")+(rail.holding?"Installed":"Ready")+" Position="+std::to_string(rail.x)+","+std::to_string(rail.y));
        } else if(s.effect=="crossflash") {
            cross.settings=s;cross.x=cursor.x-left;cross.y=cursor.y-top;cross.ready=true;cross.pulling=copilotHold.held;
            cross.until=time+8;cross.credit=0;cross.collected=0;cross.arm=0;
            log(std::string("Cross=")+(cross.pulling?"Gathering":"Ready"));
        } else if(s.effect=="blackhole") {
            blackHole.settings=s;blackHole.x=cursor.x-left;blackHole.y=cursor.y-top;blackHole.captured=0;
            blackHole.ready=true;blackHole.pulling=copilotHold.held;blackHole.until=time+8;
            blackHole.lastFeed=time;blackHole.initialRemaining=s.blackHoleCount;blackHole.spawnCredit=0;blackHole.spawnCell=0;blackHole.spawnCells.clear();
            std::vector<RECT> displays;EnumDisplayMonitors(NULL,NULL,monitors,reinterpret_cast<LPARAM>(&displays));
            for(const RECT& r:displays) {
                int nx=std::max(2,int(std::ceil((r.right-r.left)/280.))),ny=std::max(2,int(std::ceil((r.bottom-r.top)/280.)));
                double w=(r.right-r.left-16.)/nx,h=(r.bottom-r.top-16.)/ny;
                for(int y=0;y<ny;++y)for(int x=0;x<nx;++x)blackHole.spawnCells.push_back({r.left-left+8+x*w,r.top-top+8+y*h,r.left-left+8+(x+1)*w,r.top-top+8+(y+1)*h});
            }
            std::shuffle(blackHole.spawnCells.begin(),blackHole.spawnCells.end(),generator);
            log(std::string("BlackHole=")+(blackHole.pulling?"Pulling":"Ready"));
        } else if(s.effect=="pendulum") {
            Bounds b=monitor(cursor.x-left,cursor.y-top);pendulum.settings=s;
            pendulum.length=std::min(s.pendulumLength,std::max(40.,std::min((b.right-b.left)*.5-20,b.bottom-b.top-40)));
            pendulum.restLength=pendulum.length;pendulum.radialSpeed=0;
            pendulum.anchorX=cursor.x-left;pendulum.anchorY=cursor.y-top;
            pendulum.anchorVX=mouseVX;pendulum.anchorVY=mouseVY;
            pendulum.angle=0;pendulum.omega=0;pendulum.last=time;pendulum.until=time+8;
            pendulum.ready=true;pendulum.hanging=copilotHold.held;
            log(std::string("Pendulum=")+(pendulum.hanging?"Hanging":"Ready"));
        } else if(s.fire=="slingshot") {
            sling.settings=s;sling.bounds=monitor(cursor.x-left,cursor.y-top);
            sling.bounds.left+=13;sling.bounds.right-=13;sling.bounds.top+=13;sling.bounds.bottom-=13;
            sling.anchorX=clamp(cursor.x-left,sling.bounds.left,sling.bounds.right);
            sling.anchorY=clamp(cursor.y-top,sling.bounds.top,sling.bounds.bottom);
            sling.pouchX=sling.anchorX;sling.pouchY=sling.anchorY;sling.ready=true;sling.pulling=copilotHold.held;sling.until=time+8;
            log(std::string("Sling=")+(sling.pulling?"Pulling":"Ready")+" Anchor="+std::to_string(sling.anchorX)+","+std::to_string(sling.anchorY));
        } else if(s.fire=="instant") {
            if(time-started<.08) pending.push_back({double(cursor.x-left),double(cursor.y-top),started+.08,s});
            else launch(s,cursor.x-left,cursor.y-top,mouseVX,mouseVY,time);
        } else {
            armed=s;isArmed=true;armedUntil=time+8;leftDown=down(VK_LBUTTON);middleDown=down(VK_MBUTTON);spaceDown=down(VK_SPACE);
        }
        trim();invalidate();
    }
    void trim() { if(particles.size()>12000) particles.erase(particles.begin(),particles.end()-12000); }
    void addBlackHoleStars(int count,double time) {
        for(int i=0;i<count;++i) {
            if(blackHole.spawnCells.empty())return;
            Particle p;bool found=false;
            for(size_t attempt=0;attempt<blackHole.spawnCells.size()*2&&!found;++attempt) {
                if(blackHole.spawnCell>=blackHole.spawnCells.size()){blackHole.spawnCell=0;std::shuffle(blackHole.spawnCells.begin(),blackHole.spawnCells.end(),generator);}
                const Bounds& b=blackHole.spawnCells[blackHole.spawnCell++];
                for(int retry=0;retry<12&&!found;++retry) {
                    p.x=b.left+random01()*(b.right-b.left);p.y=b.top+random01()*(b.bottom-b.top);
                    found=std::hypot(p.x-blackHole.x,p.y-blackHole.y)>blackHole.radius()+24;
                }
            }
            if(!found)continue;
            p.kind=3;
            double dx=p.x-blackHole.x,dy=p.y-blackHole.y,d=std::max(1.,std::hypot(dx,dy)),circular=blackHole.circularSpeed(d);
            bool orbit=random01()<blackHole.settings.blackHoleOrbitRatio;double factor=orbit?.85+random01()*.3:.15+random01()*.45;
            double direction=random01()<.15?-1:1,radial=(random01()-.5)*circular*(orbit?.08:.2);
            p.vx=-dy/d*circular*factor*direction+dx/d*radial;p.vy=dx/d*circular*factor*direction+dy/d*radial;
            p.size=1.5+random01()*3;p.born=time;p.life=60;p.color=colorFor(blackHole.settings.palette);particles.push_back(p);
        }
        trim();
    }
    void feedBlackHole(double time) {
        if(!blackHole.pulling)return;
        double dt=clamp(time-blackHole.lastFeed,0,.1);blackHole.lastFeed=time;
        double initial=std::min(blackHole.initialRemaining,blackHole.settings.blackHoleCount/.8*dt);
        blackHole.initialRemaining-=initial;
        blackHole.spawnCredit+=initial+blackHole.settings.blackHoleFeed/.35*dt;
        int due=int(std::floor(blackHole.spawnCredit+1e-9));blackHole.spawnCredit-=due;
        size_t live=std::count_if(particles.begin(),particles.end(),[](const Particle& p){return p.kind==3;});
        if(live<1000&&due>0)addBlackHoleStars(std::min(int(1000-live),due),time);
    }
    void releasePendulum(double time,const POINT* position=NULL) {
        if(!pendulum.ready||!pendulum.hanging)return;
        if(position)pendulum.follow(position->x-left,position->y-top,time-pendulum.last);
        else pendulum.step(time-pendulum.last);
        pendulum.last=time;
        Settings shot=pendulum.settings;shot.speed=1;
        log("Pendulum=Release Position="+std::to_string(pendulum.x())+","+std::to_string(pendulum.y())+" Velocity="+std::to_string(pendulum.vx())+","+std::to_string(pendulum.vy())+" Pivot="+std::to_string(pendulum.anchorX)+","+std::to_string(pendulum.anchorY)+" PivotVelocity="+std::to_string(pendulum.anchorVX)+","+std::to_string(pendulum.anchorVY)+" Length="+std::to_string(pendulum.length)+" Rest="+std::to_string(pendulum.restLength)+" RadialVelocity="+std::to_string(pendulum.radialSpeed));
        launch(shot,pendulum.x(),pendulum.y(),pendulum.vx(),pendulum.vy(),time);
        pendulum.ready=false;pendulum.hanging=false;invalidate();
    }
    void releaseBlackHole(double cx,double cy,double time) {
        if(!blackHole.ready||!blackHole.pulling)return;
        blackHole.x=cx;blackHole.y=cy;
        Bounds b=monitor(cx,cy);b.left+=8;b.top+=8;b.right-=8;b.bottom-=8;
        double g=blackHole.settings.gravityEnabled?blackHole.settings.gravity:0;
        for(Particle& p:particles)if(p.kind==3) {
            double angle=std::atan2(p.y-cy,p.x-cx)+(random01()-.5)*.7;
            double oldVX=p.vx,oldVY=p.vy;setVariedBlast(p,angle,time);p.vx+=oldVX*.35;p.vy+=oldVY*.35;p.fall=g;
            Bounds own=monitor(p.x,p.y);own.left+=8;own.top+=8;own.right-=8;own.bottom-=8;
            p.confined=true;p.bounds=own;p.x=clamp(p.x,own.left,own.right);p.y=clamp(p.y,own.top,own.bottom);
        }
        int count=blackHole.captured;
        if(count>0)collectedBursts.push_back({count,clamp(cx,b.left,b.right),clamp(cy,b.top,b.bottom),b,blackHole.settings,blackHole.radius(),random01()*6.28318530718});
        emitCollectedBursts(time);shocks.push_back({cx,cy,time,colorForFixed(blackHole.settings.palette)});
        log("BlackHole=Release Captured="+std::to_string(blackHole.captured)+" Burst="+std::to_string(count));
        blackHole.ready=false;blackHole.pulling=false;invalidate();
    }
    void feedCross(double dt,double time) {
        if(!cross.pulling)return;
        cross.credit+=cross.settings.crossRate*clamp(dt,0,.1);
        int count=int(std::floor(cross.credit+1e-9));cross.credit-=count;
        for(int i=0;i<count;++i) {
            Particle p;p.kind=4;p.arm=cross.arm++%4;p.travel=cross.settings.crossReach*(.75+random01()*.25);
            int dx=p.arm==0?1:p.arm==1?-1:0,dy=p.arm==2?1:p.arm==3?-1:0;
            p.x=cross.x+dx*p.travel;p.y=cross.y+dy*p.travel;p.size=2+random01()*2;p.born=time;p.life=3;p.color=colorForFixed(cross.settings.palette);particles.push_back(p);
        }
        trim();
    }
    void releaseCross(double cx,double cy,double time) {
        if(!cross.ready||!cross.pulling)return;
        cross.x=cx;cross.y=cy;
        particles.erase(std::remove_if(particles.begin(),particles.end(),[](const Particle& p){return p.kind==4;}),particles.end());
        Bounds b=monitor(cx,cy);b.left+=8;b.top+=8;b.right-=8;b.bottom-=8;
        int count=cross.settings.crossBurst+std::min(cross.settings.crossBurst,cross.collected);
        std::vector<Particle> added;double g=cross.settings.gravityEnabled?cross.settings.gravity:0;
        sparks(added,clamp(cx,b.left,b.right),clamp(cy,b.top,b.bottom),0,0,count,time,false,cross.settings.palette,RGB(255,255,255),&b,0,g,true);
        double originX=clamp(cx,b.left,b.right),originY=clamp(cy,b.top,b.bottom);
        for(size_t i=0;i<added.size();++i) {
            int arm=int(i%4),dx=arm==0?1:arm==1?-1:0,dy=arm==2?1:arm==3?-1:0;
            double room=arm==0?b.right-originX:arm==1?originX-b.left:arm==2?b.bottom-originY:originY-b.top;
            double along=random01()*std::min(cross.settings.crossReach*.9,std::max(0.,room));
            added[i].x=originX+dx*along;added[i].y=originY+dy*along;
            double angle=std::atan2(double(dy),double(dx))+(random01()-.5)*.45,speed=1000+random01()*1000;
            added[i].vx=std::cos(angle)*speed;added[i].vy=std::sin(angle)*speed;
        }
        particles.insert(particles.end(),added.begin(),added.end());trim();
        shocks.push_back({cx,cy,time,RGB(255,255,255)});cross.flashAt=time;cross.ready=false;cross.pulling=false;
        log("Cross=Release Collected="+std::to_string(cross.collected)+" Burst="+std::to_string(count));invalidate();
    }
    bool blocksWheel()const{return rail.ready&&railInputHeld&&!cancelUntilUp&&!closing;}
    void chargeRail(int delta) {
        // Windows may synthesize a wheel event for the same two-finger movement.
        if(padRail&&padContacts.size()==2&&now()-padLastReport<.25)return;
        chargeRailAmount(delta);
    }
    void chargeRailAmount(int delta) {
        if(!rail.ready||!rail.holding||cancelUntilUp)return;
        rail.charge(delta);for(auto& gun:extraRails)gun.charge(delta);log("Rail=Charge Delta="+std::to_string(delta)+" Energy="+std::to_string(rail.energy));invalidate();
    }
    void placeRail(const POINT& point) {
        if(!blocksWheel()||extraRails.size()>=15)return;
        RailState gun=rail;gun.x=point.x-left;gun.y=point.y-top;gun.aim(gun.x+1,gun.y);
        extraRails.push_back(gun);log("Rail=Placed Count="+std::to_string(extraRails.size()+1));invalidate();
    }
    void releaseRail(double cx,double cy,double time) {
        fireRail(rail,cx,cy,time);for(auto& gun:extraRails)fireRail(gun,cx,cy,time);
        if(padRail){padReady=false;padContacts.clear();railPadHeads.clear();railPadCredit=0;}
    }
    void fireRail(RailState& rail,double cx,double cy,double time) {
        if(!rail.ready||!rail.holding)return;
        rail.aim(cx,cy);rail.ready=false;rail.holding=false;rail.visibleUntil=time+.6;
        if(rail.energy<=0){log("Rail=DryRelease");invalidate();return;}
        rail.firedAt=time;
        POINT muzzle=rail.world(178,0);Settings shot=rail.settings;shot.speed=1;shot.gravityEnabled=false;shot.fuse=5;
        shot.clusterCount=std::min(1200,shot.railImpact+int(rail.energy*rail.energy*.04));
        double speed=(3000+27000*std::pow(rail.energy/100,1.3))*shot.railPower;
        launch(shot,muzzle.x,muzzle.y,rail.dx*speed,rail.dy*speed,time);
        Cluster& slug=clusters.back();slug.railSlug=true;slug.railEnergy=rail.energy;
        double endpointTime=std::min(slug.edgeTime,shot.fuse);
        railTraces.push_back({slug.originX,slug.originY,slug.originX+slug.vx*endpointTime,slug.originY+slug.vy*endpointTime,time,rail.energy,colorForFixed(shot.palette)});
        log("Rail=Fire Energy="+std::to_string(rail.energy)+" Speed="+std::to_string(speed)+" Direction="+std::to_string(rail.dx)+","+std::to_string(rail.dy));invalidate();
    }
    void emitCollectedBursts(double time) {
        while(!collectedBursts.empty()) {
            int room=std::max(0,12000-int(particles.size()));if(room==0)return;
            CollectedBurst& burst=collectedBursts.front();int count=std::min(room,burst.remaining);
            std::vector<Particle> added;double g=burst.settings.gravityEnabled?burst.settings.gravity:0;
            sparks(added,burst.x,burst.y,0,0,count,time,false,burst.settings.palette,RGB(255,255,255),&burst.bounds,0,g,true);
            for(Particle& p:added) {
                bool jet=random01()<.15;double angle=jet?burst.jetAngle+(random01()<.5?0:3.14159265359)+(random01()-.5)*.18:random01()*6.28318530718;
                double radius=random01()<.6?std::sqrt(random01())*burst.radius*.85:burst.radius*(.75+random01()*.55);
                p.x=clamp(burst.x+std::cos(angle)*radius,burst.bounds.left,burst.bounds.right);p.y=clamp(burst.y+std::sin(angle)*radius,burst.bounds.top,burst.bounds.bottom);
                setVariedBlast(p,angle+(jet?0:(random01()-.5)*.8),time,jet);p.fall=g;
            }
            particles.insert(particles.end(),added.begin(),added.end());burst.remaining-=count;
            log("CollectedBurst=Emitted Count="+std::to_string(count)+" Remaining="+std::to_string(burst.remaining));
            if(burst.remaining==0)collectedBursts.erase(collectedBursts.begin());
        }
    }
    void setVariedBlast(Particle& p,double angle,double time,bool jet=false) {
        double group=jet?.95:random01(),speed;
        if(group<.4){speed=250+random01()*600;p.life=2.6+random01()*1.2;p.drag=.08+random01()*.17;}
        else if(group<.8){speed=850+random01()*1000;p.life=2.1+random01()*1.1;p.drag=.15+random01()*.25;}
        else{speed=1850+random01()*1750;p.life=1.4+random01()*1.2;p.drag=.25+random01()*.4;}
        p.kind=2;p.vx=std::cos(angle)*speed;p.vy=std::sin(angle)*speed;p.born=time;p.size=1.2+random01()*4.8;
    }
    bool stepOrbit(Particle& p,double dt) {
        while(dt>1e-9) {
            double h=std::min(dt,1./120.),ox=p.x,oy=p.y,dx=blackHole.x-p.x,dy=blackHole.y-p.y,d=std::hypot(dx,dy);
            if(d<=blackHole.radius())return true;
            double a=blackHole.acceleration(d);p.vx+=dx/d*a*h*.5;p.vy+=dy/d*a*h*.5;
            p.x+=p.vx*h;p.y+=p.vy*h;dx=blackHole.x-p.x;dy=blackHole.y-p.y;d=std::max(1e-9,std::hypot(dx,dy));
            a=blackHole.acceleration(d);p.vx+=dx/d*a*h*.5;p.vy+=dy/d*a*h*.5;
            double drag=std::exp(-blackHole.settings.blackHoleOrbitDamping*h);p.vx*=drag;p.vy*=drag;
            double hx,hy;if(collision(ox,oy,p.x,p.y,blackHole.x,blackHole.y,blackHole.radius(),hx,hy))return true;
            dt-=h;
        }
        return false;
    }
    void releaseSling(double cx,double cy,double time) {
        if(!sling.ready||!sling.pulling)return;
        sling.pull(cx,cy);Settings shot=sling.settings;shot.speed=1;
        log("Sling=Release Pull="+std::to_string(std::hypot(sling.pouchX-sling.anchorX,sling.pouchY-sling.anchorY))+" Anchor="+std::to_string(sling.anchorX)+","+std::to_string(sling.anchorY)+" Pouch="+std::to_string(sling.pouchX)+","+std::to_string(sling.pouchY)+" Power="+std::to_string(sling.settings.slingPower));
        launch(shot,sling.pouchX,sling.pouchY,sling.vx(),sling.vy(),time);
        sling.ready=false;sling.pulling=false;invalidate();
    }
    void openSettings(){
        std::wstring exe=directory+L"이게 코파일럿보다 낫다.exe";
        HWND existing=FindWindowW(NULL,L"이게 코파일럿보다 낫다 · 효과 설정");
        if(existing){DWORD pid=0;GetWindowThreadProcessId(existing,&pid);
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
            wchar_t path[32768];DWORD length=32768;bool same=process&&QueryFullProcessImageNameW(process,0,path,&length)&&
                (_wcsicmp(path,exe.c_str())==0||_wcsicmp(path,(directory+L"차라리 이거.exe").c_str())==0);
            if(process)CloseHandle(process);
            if(same){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);log("SettingsShortcut=Reused");return;}}
        std::wstring command=L"\""+exe+L"\"";
        STARTUPINFOW startup={};startup.cb=sizeof(startup);startup.dwFlags=STARTF_FORCEOFFFEEDBACK;
        PROCESS_INFORMATION process={};
        if(!CreateProcessW(exe.c_str(),&command[0],NULL,NULL,FALSE,0,NULL,directory.c_str(),&startup,&process)){
            log("SettingsShortcutError="+std::to_string(GetLastError()));return;}
        AllowSetForegroundWindow(process.dwProcessId);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        log("SettingsShortcut=Opened");
    }
    void requestSettings(){
        if(settingsPending||closing)return;
        settingsPending=true;log("SettingsShortcut=Requested");
        if(copilotHold.held)finish("Escape");
        else {openSettings();finish("SettingsShortcut");}
    }
    void copilot(bool up,DWORD eventTime,const POINT* position=NULL) {
        if(closing)return;
        bool wasHeld=copilotHold.held;
        bool fresh=copilotHold.update(up);
        if(cancelUntilUp) { if(up){if(settingsPending)openSettings();finish("EscapeReleased");}return; }
        if(up) {
            log("Copilot=UP Held="+std::to_string(wasHeld)+" HoldMs="+std::to_string(wasHeld?(now()-holdStarted)*1000:0)+" EventTick="+std::to_string(eventTime));
            POINT cursor;if(position)cursor=*position;else GetCursorPos(&cursor);releaseSling(cursor.x-left,cursor.y-top,now());
            releasePendulum(now(),&cursor);releaseBlackHole(cursor.x-left,cursor.y-top,now());
            releaseCross(cursor.x-left,cursor.y-top,now());
            releaseRail(cursor.x-left,cursor.y-top,now());
            releaseHoming(cursor.x-left,cursor.y-top,now());
            if(dice.visible&&dice.holding){dice.holding=false;dice.until=now()+1.5;log("Dice=Release");}
        } else if(fresh) {
            if(drawing){if(now()-drawOpened>.3)finish("DrawingToggle");return;}
            holdStarted=now();log("Copilot=DOWN EventTick="+std::to_string(eventTime));
            try { Settings next=homingReady?homingSettings:sling.ready?sling.settings:pendulum.ready?pendulum.settings:blackHole.ready?blackHole.settings:cross.ready?cross.settings:rail.ready?rail.settings:dice.visible?dice.settings:Settings::from(readFile(directory+L"settings.json"));trigger(next,position); }
            catch(const std::exception& error) { log(std::string("SettingsError=")+error.what()); }
        } else log("Copilot=REPEAT Count="+std::to_string(copilotHold.repeats));
    }
    void sparks(std::vector<Particle>& added,double x,double y,double inheritedX,double inheritedY,int count,double time,bool small,const std::string& palette,COLORREF color,const Bounds* boundary=NULL,unsigned sides=0,double fall=70,bool explosive=false) {
        for(int i=0;i<count;++i) {
            double angle=random01()*6.28318530718,speed=(explosive?1000:small?80:70)+random01()*(explosive?1000:small?180:330);
            Particle p;p.kind=2;p.x=x;p.y=y;p.vx=std::cos(angle)*speed+inheritedX;p.vy=std::sin(angle)*speed+inheritedY;p.size=2+random01()*2;p.born=time;p.life=small?.35+random01()*.3:1+random01()*.9;p.color=small?color:colorFor(palette);p.fall=fall;
            if(explosive){p.life=2.2+random01();p.drag=.45;}
            if(boundary) {
                p.confined=true;p.bounds=*boundary;
                if(sides&1)p.vx=std::abs(p.vx);if(sides&2)p.vx=-std::abs(p.vx);
                if(sides&4)p.vy=std::abs(p.vy);if(sides&8)p.vy=-std::abs(p.vy);
            }
            added.push_back(p);
        }
    }
    void railImpact(std::vector<Particle>& added,const Cluster& c,double time,bool edge){
        size_t first=added.size();double charge=clamp(c.railEnergy/100,0,1);
        sparks(added,c.x,c.y,0,0,c.settings.clusterCount,time,false,c.settings.palette,c.color,
               edge?&c.bounds:NULL,c.edgeSides,30);
        for(size_t i=first;i<added.size();++i){
            Particle& p=added[i];double angle=std::atan2(p.vy,p.vx);
            double speed=80+charge*1000+random01()*(120+charge*1800);
            p.vx=std::cos(angle)*speed;p.vy=std::sin(angle)*speed;
            p.size=1+charge*2+random01()*(1+charge*3);
            p.life=.45+charge*1.5+random01()*(.35+charge*.7);
            p.drag=2.4-charge*1.8;p.fall=30+charge*100;
            p.railStyle=charge<.3?1:charge<.7?2:(i%4==0?3:2);
            if(charge>=.7&&i%4==0)p.color=RGB(255,245,215);
        }
        if(charge>=.7)shocks.push_back({c.x,c.y,time,colorForFixed(c.settings.palette)});
    }
    void physics(double time,double dt,double cx,double cy) {
        std::vector<Particle> added;
        for(size_t i=clusters.size();i-- >0;) {
            Cluster& c=clusters[i];if(!c.trajectoryReady)trajectory(c);
            double age=std::max(0.,time-c.born),eventTime=std::min(c.edgeTime,c.settings.fuse);
            bool edge=c.edgeTime<=c.settings.fuse;
            double g=c.settings.gravityEnabled?c.settings.gravity:0;
            double travel=std::min(age,eventTime);c.x=c.originX+c.vx*travel;c.y=c.originY+c.vy*travel+.5*g*travel*travel;
            if(age>=eventTime) {
                if(edge) { c.x=c.edgeX;c.y=c.edgeY; }
                ++explosions;log(std::string("Explosion=")+(edge?"Edge":"Timer")+" Position="+std::to_string(c.x)+","+std::to_string(c.y)+" FlightSeconds="+std::to_string(eventTime));
                if(c.railSlug)railImpact(added,c,time,edge);
                else sparks(added,c.x,c.y,edge?0:c.vx*.2,edge?0:(c.vy+g*eventTime)*.2,c.settings.clusterCount,time,false,c.settings.palette,c.color,edge?&c.bounds:NULL,c.edgeSides);
                clusters[i]=std::move(clusters.back());clusters.pop_back();
            }
        }
        for(size_t i=particles.size();i-- >0;) {
            Particle& p=particles[i];bool remove=time-p.born>=p.life;
            if(p.kind==3) {
                bool captured=!remove&&stepOrbit(p,dt);if(captured)++blackHole.captured;
                if(remove||captured){particles[i]=particles.back();particles.pop_back();}
                continue;
            }
            if(p.kind==4) {
                p.travel-=(1200+cross.charge()*600)*dt;
                int dx=p.arm==0?1:p.arm==1?-1:0,dy=p.arm==2?1:p.arm==3?-1:0;
                p.x=cross.x+dx*p.travel;p.y=cross.y+dy*p.travel;p.vx=-dx*(1200+cross.charge()*600);p.vy=-dy*(1200+cross.charge()*600);
                if(p.travel<=10||remove){if(p.travel<=10)++cross.collected;particles[i]=particles.back();particles.pop_back();}
                continue;
            }
            if(!remove) {
                double ox=p.x,oy=p.y;
                if(p.kind==1||p.kind==3) {
                    bool hole=p.kind==3;double centerX=hole?blackHole.x:cx,centerY=hole?blackHole.y:cy;
                    double dx=centerX-p.x,dy=centerY-p.y,distance=std::max(1.,std::hypot(dx,dy));
                    double strength=hole?blackHole.settings.blackHoleStrength:p.strength;
                    double force=hole?blackHole.acceleration(distance):std::min(2200*strength,8500000*strength/(distance*distance+2400));
                    p.vx+=dx/distance*force*dt;p.vy+=dy/distance*force*dt;
                }
                double drag=std::exp(-(p.kind==0?.45:p.kind==1?.22:p.kind==3?.65:p.drag)*dt);p.vx*=drag;p.vy=p.vy*drag+(p.kind==0?620:p.kind==2?p.fall:0)*dt;
                p.x+=p.vx*dt;p.y+=p.vy*dt;p.angle+=p.spin*dt;
                if(p.confined) {
                    if(p.x<p.bounds.left) { p.x=p.bounds.left;p.vx=std::abs(p.vx)*.45; }
                    if(p.x>p.bounds.right) { p.x=p.bounds.right;p.vx=-std::abs(p.vx)*.45; }
                    if(p.y<p.bounds.top) { p.y=p.bounds.top;p.vy=std::abs(p.vy)*.45; }
                    if(p.y>p.bounds.bottom) { p.y=p.bounds.bottom;p.vy=-std::abs(p.vy)*.45; }
                }
                double hx,hy;
                if(p.kind==1&&collision(ox,oy,p.x,p.y,cx,cy,p.radius,hx,hy)) { remove=true;++collisions;sparks(added,hx,hy,0,0,9,time,true,"",p.color); }
                if(p.kind==3&&collision(ox,oy,p.x,p.y,blackHole.x,blackHole.y,blackHole.radius(),hx,hy)){remove=true;++blackHole.captured;}
            }
            if(remove) { particles[i]=particles.back();particles.pop_back(); }
        }
        particles.insert(particles.end(),added.begin(),added.end());trim();
    }
    void input(bool l,bool m,bool sp,double cx,double cy,double time) {
        if(isArmed) {
            if(time>=armedUntil) isArmed=false;
            else if((armed.fire=="left"&&l&&!leftDown)||(armed.fire=="middle"&&m&&!middleDown)||(armed.fire=="space"&&sp&&!spaceDown)) { launch(armed,cx,cy,mouseVX,mouseVY,time);armedUntil=time+8;log("InputFire="+armed.fire); }
        }
        leftDown=l;middleDown=m;spaceDown=sp;
    }
    void finish(const char* why) {
        if(closing)return;
        guidedShots.clear();homingReady=false;homingHolding=false;
        padReady=false;padRail=false;railPadHeads.clear();railPadCredit=0;padContacts.clear();drawing=false;penDown=false;
        if(copilotHold.held&&std::string(why)=="Escape") {
            particles.clear();clusters.clear();pending.clear();shocks.clear();collectedBursts.clear();railTraces.clear();extraRails.clear();dice.visible=false;rail.ready=false;rail.holding=false;rail.visibleUntil=-100;isArmed=false;sling.ready=false;sling.pulling=false;pendulum.ready=false;pendulum.hanging=false;blackHole.ready=false;blackHole.pulling=false;cross.ready=false;cross.pulling=false;cross.flashAt=-100;cancelUntilUp=true;
            if(window)ShowWindow(window,SW_HIDE);log("Escape=Cancelled WaitForUp");return;
        }
        closing=true;
        if(keyboardHook) { UnhookWindowsHookEx(keyboardHook);keyboardHook=NULL; }
        if(mouseHook){UnhookWindowsHookEx(mouseHook);mouseHook=NULL;}
        log(std::string("Exit=")+why+" Collisions="+std::to_string(collisions)+" Explosions="+std::to_string(explosions));
        particles.clear();clusters.clear();pending.clear();shocks.clear();collectedBursts.clear();railTraces.clear();extraRails.clear();dice.visible=false;rail.ready=false;rail.holding=false;rail.visibleUntil=-100;isArmed=false;sling.ready=false;sling.pulling=false;pendulum.ready=false;pendulum.hanging=false;blackHole.ready=false;blackHole.pulling=false;cross.ready=false;cross.pulling=false;cross.flashAt=-100;
        if(window) { KillTimer(window,1);DestroyWindow(window); }
    }
    void tick() {
        if(cancelUntilUp)return;
        if(down(VK_ESCAPE)) { finish("Escape");return; }
        double time=now(),elapsed=time-previous,dt=std::min(.04,elapsed);previous=time;
        POINT cursor;GetCursorPos(&cursor);
        if(elapsed>.001) { double blend=1-std::exp(-elapsed/.045);mouseVX+=((cursor.x-previousCursor.x)/elapsed-mouseVX)*blend;mouseVY+=((cursor.y-previousCursor.y)/elapsed-mouseVY)*blend; }
        previousCursor=cursor;double cx=cursor.x-left,cy=cursor.y-top;
        padStep(time,dt);
        if(drawing){if(drawDirty){invalidate();drawDirty=false;}return;}
        if(rail.ready){if(rail.holding)rail.aim(cx,cy);else if(time>=rail.until)rail.ready=false;}
        for(auto& gun:extraRails)if(gun.ready&&gun.holding)gun.aim(cx,cy);
        if(dice.visible){bool was=dice.settled;dice.step(cx,cy,dt,time);Bounds b=monitor(cx,cy);dice.x=clamp(dice.x,b.left+110,b.right-110);dice.y=clamp(dice.y,b.top+110,b.bottom-110);if(dice.settled&&!was)log("Dice=Settled");}
        railTraces.erase(std::remove_if(railTraces.begin(),railTraces.end(),[&](const RailTrace& beam){return time-beam.born>=.16+beam.energy*.003;}),railTraces.end());
        if(cross.ready){cross.x=cx;cross.y=cy;if(cross.pulling)feedCross(elapsed,time);else if(time>=cross.until)cross.ready=false;}
        if(sling.ready) { if(sling.pulling)sling.pull(cx,cy);else if(time>=sling.until)sling.ready=false; }
        if(pendulum.ready) {
            if(pendulum.hanging){pendulum.follow(cx,cy,time-pendulum.last);pendulum.last=time;}
            else if(time>=pendulum.until)pendulum.ready=false;
        }
        if(blackHole.ready) {
            blackHole.x=cx;blackHole.y=cy;
            if(blackHole.pulling)feedBlackHole(time);
            else if(time>=blackHole.until)blackHole.ready=false;
        }
        shocks.erase(std::remove_if(shocks.begin(),shocks.end(),[&](const Shock& wave){return time-wave.born>=.7;}),shocks.end());
        input(down(VK_LBUTTON),down(VK_MBUTTON),down(VK_SPACE),cx,cy,time);
        for(size_t i=pending.size();i-- >0;) if(time>=pending[i].due) { Pending p=pending[i];pending.erase(pending.begin()+i);launch(p.settings,p.x,p.y,mouseVX,mouseVY,time); }
        stepHoming(time,dt);
        if(homingReady&&!homingHolding&&time>=homingUntil)homingReady=false;
        physics(time,dt,cx,cy);
        emitCollectedBursts(time);
        if(guidedShots.empty()&&!homingReady&&particles.empty()&&clusters.empty()&&pending.empty()&&shocks.empty()&&collectedBursts.empty()&&railTraces.empty()&&!isArmed&&!sling.ready&&!pendulum.ready&&!blackHole.ready&&!cross.ready&&!dice.visible&&!rail.ready&&time>=rail.visibleUntil&&!padReady&&!copilotHold.held) { finish("Empty");return; }
        invalidate();
    }
    void invalidate() {
        RECT bounds={};
        if(drawing)bounds=board;
        if(homingReady)bounds={LONG(homingX-20),LONG(homingY-20),LONG(homingX+380),LONG(homingY+70)};
        auto include=[&](double x,double y,double vx,double vy) {
            RECT r={LONG(std::floor(std::min(x,x-vx*.06)-20)),LONG(std::floor(std::min(y,y-vy*.06)-20)),LONG(std::ceil(std::max(x,x-vx*.06)+20)),LONG(std::ceil(std::max(y,y-vy*.06)+20))};
            if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);
        };
        for(const PadContact& c:padContacts)if(!padRail)include(padBounds.left+24+c.x*(padBounds.right-padBounds.left-48),padBounds.top+24+c.y*(padBounds.bottom-padBounds.top-48),0,0);
        for(const GuidedShot& m:guidedShots)if(now()>=m.born)include(m.x,m.y,m.vx,m.vy);
        for(const Particle& p:particles) if(p.x>-150&&p.y>-150&&p.x<width+150&&p.y<height+150)include(p.x,p.y,p.vx,p.vy);
        for(const Cluster& c:clusters)include(c.x,c.y,c.vx,c.vy+(c.settings.gravityEnabled?c.settings.gravity*std::max(0.,now()-c.born):0));
        if(rail.ready||now()<rail.visibleUntil){RECT r={LONG(rail.x-340),LONG(rail.y-340),LONG(rail.x+340),LONG(rail.y+340)};if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);}
        for(const auto& gun:extraRails)if(gun.ready||now()<gun.visibleUntil){RECT r={LONG(gun.x-340),LONG(gun.y-340),LONG(gun.x+340),LONG(gun.y+340)};if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);}
        if(dice.visible){RECT r={LONG(dice.x-150),LONG(dice.y-150),LONG(dice.x+150),LONG(dice.y+150)};if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);}
        for(const RailTrace& beam:railTraces){include(beam.x,beam.y,0,0);include(beam.endX,beam.endY,0,0);}
        if(cross.ready||now()-cross.flashAt<.18) {
            double reach=cross.settings.crossReach*(now()-cross.flashAt<.18?1.5:1)+30;
            RECT r={LONG(cross.x-reach),LONG(cross.y-reach),LONG(cross.x+reach),LONG(cross.y+reach)};
            if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);
        }
        if(pendulum.ready) {
            include(pendulum.anchorX,pendulum.anchorY,0,0);include(pendulum.x(),pendulum.y(),0,0);
            RECT r={LONG(pendulum.x()-20),LONG(pendulum.y()-20),LONG(pendulum.x()+285),LONG(pendulum.y()+65)};UnionRect(&bounds,&bounds,&r);
            include(pendulum.x()+pendulum.vx()*.08,pendulum.y()+pendulum.vy()*.08,0,0);
        }
        if(blackHole.ready) {
            double radius=blackHole.radius(),rx=std::ceil(radius*2.6+20),ry=std::ceil(radius+70);
            RECT r={LONG(blackHole.x-rx),LONG(blackHole.y-ry),LONG(blackHole.x+std::max(rx,radius+300)),LONG(blackHole.y+ry)};
            if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);
        }
        for(const Shock& wave:shocks) {double radius=15+std::min(.7,now()-wave.born)/.7*210;include(wave.x-radius,wave.y-radius,0,0);include(wave.x+radius,wave.y+radius,0,0);}
        if(sling.ready) {
            POINT cursor;GetCursorPos(&cursor);
            double x=sling.pulling?sling.pouchX:cursor.x-left,y=sling.pulling?sling.pouchY:cursor.y-top;
            include(sling.anchorX,sling.anchorY,0,0);include(x,y,0,0);
            RECT r={LONG(x-20),LONG(y-20),LONG(x+310),LONG(y+85)};if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r);
            if(sling.pulling)for(int i=1;i<=7;++i) { double t=.02*i;include(x+sling.vx()*t,y+sling.vy()*t+(sling.settings.gravityEnabled?.5*sling.settings.gravity*t*t:0),0,0); }
        }
        if(isArmed) { POINT cursor;GetCursorPos(&cursor);RECT r={cursor.x-left-20,cursor.y-top-20,cursor.x-left+280,cursor.y-top+80};if(IsRectEmpty(&bounds))bounds=r;else UnionRect(&bounds,&bounds,&r); }
        RECT all={0,0,width,height};IntersectRect(&bounds,&bounds,&all);RECT dirty;UnionRect(&dirty,&bounds,&previousBounds);previousBounds=bounds;
        if(window&&!IsRectEmpty(&dirty)){UnionRect(&pendingPaint,&pendingPaint,&dirty);InvalidateRect(window,&dirty,FALSE);}
    }
    void line(double x,double y,double nx,double ny,COLORREF color) { SetDCPenColor(buffer,color);MoveToEx(buffer,int(x),int(y),NULL);LineTo(buffer,int(nx),int(ny)); }
    void star(double x,double y,double size,COLORREF color) { line(x-size,y,x+size,y,color);line(x,y-size,x,y+size,color); }
    void clusterDraw(double x,double y,double vx,double vy,double age,COLORREF color) {
        line(x-vx*.06,y-vy*.06,x,y,fadeColor(color,.33));
        for(int i=0;i<22;++i) { double angle=i*2.39996+age*4,radius=std::sqrt(double(i))*2.3;star(x+std::cos(angle)*radius,y+std::sin(angle)*radius,i%3==0?3:2,color); }
    }
    void crossDraw(double x,double y,double reach,COLORREF color,double power) {
        SelectObject(buffer,GetStockObject(DC_BRUSH));
        double core=2+power*5,halo=core*5;
        auto blades=[&](double width,COLORREF shade) {
            SetDCPenColor(buffer,shade);SetDCBrushColor(buffer,shade);
            RoundRect(buffer,int(x-reach),int(y-width),int(x+reach),int(y+width),int(width*2),int(width*2));
            RoundRect(buffer,int(x-width),int(y-reach),int(x+width),int(y+reach),int(width*2),int(width*2));
        };
        for(int width=int(std::ceil(halo));width>int(core);--width) {
            double fall=clamp((halo-width)/(halo-core),0,1);
            blades(width,fadeColor(color,(.04+.85*fall*fall)*power));
        }
        blades(core,color==RGB(255,255,255)?fadeColor(color,power):RGB(255,255,255));
    }
    void thickLine(double x,double y,double ex,double ey,COLORREF color,int width) {
        HPEN pen=CreatePen(PS_SOLID,std::max(1,width),color);HGDIOBJ old=SelectObject(buffer,pen);
        MoveToEx(buffer,int(x),int(y),NULL);LineTo(buffer,int(ex),int(ey));SelectObject(buffer,old);DeleteObject(pen);
    }
    void opacityPolygon(const POINT* points,int count,double opacity=1){
        int a=int(clamp(opacity,0,1)*255);COLORREF value=RGB(a,a,a);
        SetDCBrushColor(coverageDC,value);SetDCPenColor(coverageDC,value);
        Polygon(coverageDC,points,count);
    }
    void opacityEllipse(int x1,int y1,int x2,int y2,double opacity=1){
        int a=int(clamp(opacity,0,1)*255);COLORREF value=RGB(a,a,a);
        SetDCBrushColor(coverageDC,value);SetDCPenColor(coverageDC,value);
        Ellipse(coverageDC,x1,y1,x2,y2);
    }
    void railChargeGlow(const RailState& rail,double fade,double charge,COLORREF accent){
        if(charge<=0)return;
        COLORREF light=RGB((GetRValue(accent)+255)/2,(GetGValue(accent)+255)/2,(GetBValue(accent)+255)/2);
        // Premultiply a bright tint by its opacity. No opaque coverage is added.
        for(int i=4;i>=0;--i){
            double spread=18+i*10,opacity=fade*charge*charge*.32/(i+1);
            POINT a=rail.world(40,-spread),b=rail.world(180,-spread);
            POINT c=rail.world(40,spread),d=rail.world(180,spread);
            COLORREF glow=fadeColor(light,opacity);
            thickLine(a.x,a.y,b.x,b.y,glow,int(5+charge*15));
            thickLine(c.x,c.y,d.x,d.y,glow,int(5+charge*15));
        }
    }
    void railDraw(const RailState& rail,double time) {
        double fade=rail.ready?1:clamp((rail.visibleUntil-time)/.25,0,1);
        double charge=rail.energy/100,recoil=rail.firedAt>-99?rail.energy*.2*std::exp(-(time-rail.firedAt)/.08):0;
        COLORREF accent=colorForFixed(rail.settings.palette);
        railChargeGlow(rail,fade,charge,accent);
        auto poly=[&](std::initializer_list<std::pair<double,double>> vertices,COLORREF fill,COLORREF edge){
            std::vector<POINT> points;for(const auto& v:vertices)points.push_back(rail.world(v.first,v.second,recoil));
            SetDCBrushColor(buffer,fadeColor(fill,fade));SetDCPenColor(buffer,fadeColor(edge,fade));Polygon(buffer,points.data(),int(points.size()));opacityPolygon(points.data(),int(points.size()),fade);
        };
        auto detail=[&](double a,double b,double c,double d,COLORREF color,int width=1){POINT p=rail.world(a,b,recoil),q=rail.world(c,d,recoil);thickLine(p.x,p.y,q.x,q.y,fadeColor(color,fade),width);};
        SelectObject(buffer,GetStockObject(DC_BRUSH));SetDCBrushColor(buffer,fadeColor(RGB(22,28,38),fade));SetDCPenColor(buffer,fadeColor(RGB(117,137,160),fade));
        Ellipse(buffer,int(rail.x-24),int(rail.y-20),int(rail.x+24),int(rail.y+20));
        opacityEllipse(int(rail.x-24),int(rail.y-20),int(rail.x+24),int(rail.y+20),fade);
        thickLine(rail.x-32,rail.y+20,rail.x+32,rail.y+20,fadeColor(RGB(74,90,109),fade),5);
        poly({{-60,-20},{-40,-32},{20,-32},{41,-22},{41,22},{20,32},{-40,32},{-60,20}},RGB(28,36,48),RGB(105,124,147));
        poly({{-48,-16},{-35,-24},{6,-24},{17,-14},{17,14},{6,24},{-35,24},{-48,16}},RGB(46,58,73),RGB(134,151,170));
        poly({{-13,-12},{48,-12},{62,-6},{62,6},{48,12},{-13,12}},RGB(7,13,23),RGB(73,100,124));
        poly({{37,-27},{149,-27},{178,-18},{178,-9},{37,-9}},RGB(49,62,79),RGB(153,170,190));
        poly({{37,9},{178,9},{178,18},{149,27},{37,27}},RGB(35,45,60),RGB(123,144,170));
        detail(40,-25,147,-25,RGB(208,216,227),2);detail(40,10,173,10,RGB(162,181,203),2);
        detail(43,-11,172,-11,fadeColor(accent,.3+.7*charge),3);detail(43,11,172,11,fadeColor(accent,.3+.7*charge),3);
        for(int i=0;i<5;++i){double a=49+i*23;
            poly({{a,-31},{a+9,-31},{a+9,-7},{a,-7}},RGB(21,29,40),RGB(98,117,139));
            poly({{a,7},{a+9,7},{a+9,31},{a,31}},RGB(18,26,36),RGB(79,98,123));
            detail(a+2,-28,a+2,-10,fadeColor(accent,charge>.15*i?.8:.12),2);detail(a+2,10,a+2,28,fadeColor(accent,charge>.15*i?.8:.12),2);
        }
        poly({{161,-22},{184,-17},{184,-7},{169,-7}},RGB(61,75,90),RGB(192,209,225));
        poly({{169,7},{184,7},{184,17},{161,22}},RGB(43,58,75),RGB(155,181,205));
        detail(-34,-18,5,-18,RGB(211,221,235),1);detail(-34,18,5,18,RGB(86,104,126),1);
        for(int i=0;i<10;++i){double a=-38+i*4.3;detail(a,-8,a,8,fadeColor(accent,rail.energy>=i*10?.9:.12),2);}
        if(rail.energy>0){
            for(int i=0;i<14;++i){double a=59+i*8,b=59+(i+1)*8;detail(a,std::sin(time*18+i*1.7)*5*charge,b,std::sin(time*18+(i+1)*1.7)*5*charge,fadeColor(accent,.4+.6*charge),2);}
            detail(13,0,45,0,RGB(230,248,255),3);
        }
        for(const auto& bolt:std::vector<std::pair<double,double>>{{-37,-24},{8,-24},{-37,24},{8,24},{155,-20},{155,20}}){
            POINT p=rail.world(bolt.first,bolt.second,recoil);SetDCBrushColor(buffer,fadeColor(RGB(166,185,205),fade));SetDCPenColor(buffer,fadeColor(RGB(16,23,32),fade));Ellipse(buffer,p.x-3,p.y-3,p.x+3,p.y+3);opacityEllipse(p.x-3,p.y-3,p.x+3,p.y+3,fade);
        }
        // Draw charging lights last so the gun body cannot cover them.
        if(charge>0){
            COLORREF light=RGB((GetRValue(accent)+255)/2,(GetGValue(accent)+255)/2,(GetBValue(accent)+255)/2);
            for(int i=0;i<int(8+charge*34);++i){
                double phase=time*(1.4+charge*3)+i*2.39996323;
                double radius=35+charge*105+std::sin(time*4+i)*18;
                double x=rail.x+75*rail.dx+std::cos(phase)*radius,y=rail.y+75*rail.dy+std::sin(phase)*radius*.65;
                line(x,y,x-std::cos(phase)*charge*20,y-std::sin(phase)*charge*13,fadeColor(light,fade*.8));
                star(x,y,1+charge*3,fadeColor(light,fade));
                star(x,y,1,fadeColor(RGB(255,255,255),fade));
            }
        }
        std::wstring label=rail.holding?L"에너지 "+std::to_wstring(int(std::lround(rail.energy)))+L"% · 휠 클릭 추가 · 놓으면 발사":rail.ready?L"Copilot hold → 휠로 충전":L"발사";
        SetTextColor(buffer,fadeColor(accent,fade));TextOutW(buffer,int(rail.x-95),int(rail.y+70),label.c_str(),int(label.size()));
    }
    void diceDraw() {
        struct V {double x,y,z;};
        auto rotate=[&](V v){double c=std::cos(dice.angle[0]),s=std::sin(dice.angle[0]);v={v.x,v.y*c-v.z*s,v.y*s+v.z*c};
            c=std::cos(dice.angle[1]);s=std::sin(dice.angle[1]);v={v.x*c+v.z*s,v.y,-v.x*s+v.z*c};
            c=std::cos(dice.angle[2]);s=std::sin(dice.angle[2]);return V{v.x*c-v.y*s,v.x*s+v.y*c,v.z};};
        auto project=[&](V v){return POINT{LONG(dice.x+v.x*dice.settings.diceSize*.5),LONG(dice.y+v.y*dice.settings.diceSize*.5)};};
        const V normals[6]={{0,0,1},{1,0,0},{0,1,0},{0,-1,0},{-1,0,0},{0,0,-1}};
        const V us[6]={{1,0,0},{0,0,-1},{1,0,0},{1,0,0},{0,0,1},{-1,0,0}};
        const V vs[6]={{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
        int face=1;double best=-1;
        for(int f=0;f<6;++f){V n=rotate(normals[f]);if(n.z<=.001)continue;if(n.z>best){best=n.z;face=f+1;}
            auto point=[&](double a,double b){V q={normals[f].x+us[f].x*a+vs[f].x*b,normals[f].y+us[f].y*a+vs[f].y*b,normals[f].z+us[f].z*a+vs[f].z*b};return project(rotate(q));};
            POINT quad[4]={point(-1,-1),point(1,-1),point(1,1),point(-1,1)};
            SetDCBrushColor(buffer,fadeColor(RGB(238,245,255),.5+.5*n.z));SetDCPenColor(buffer,RGB(130,170,220));Polygon(buffer,quad,4);opacityPolygon(quad,4);
            int number=f+1;std::vector<std::pair<double,double>> dots;
            if(number%2)dots.push_back({0,0});
            if(number>=2){dots.push_back({-.5,-.5});dots.push_back({.5,.5});}
            if(number>=4){dots.push_back({.5,-.5});dots.push_back({-.5,.5});}
            if(number==6){dots.push_back({-.5,0});dots.push_back({.5,0});}
            for(auto dot:dots){POINT pts[16];for(int i=0;i<16;++i){double a=i*6.28318530718/16;pts[i]=point(dot.first+std::cos(a)*.115,dot.second+std::sin(a)*.115);}
                SetDCBrushColor(buffer,RGB(29,43,68));SetDCPenColor(buffer,RGB(29,43,68));Polygon(buffer,pts,16);}
        }
        std::wstring label=dice.settled?L"결과 "+std::to_wstring(face):L"흔들어서 굴리기";
        SetTextColor(buffer,RGB(180,220,255));TextOutW(buffer,int(dice.x-60),int(dice.y+dice.settings.diceSize*.8+15),label.c_str(),int(label.size()));
    }
    void finalizeAlpha(const RECT* dirty=NULL){
        // GDI writes RGB only. RGB drawn over black already represents premultiplied
        // light intensity; supply its coverage as alpha instead of a color key.
        GdiFlush();
        double holeRadius=blackHole.ready?blackHole.radius():0;
        RECT area=dirty?*dirty:RECT{0,0,width,height};
        for(int y=area.top;y<area.bottom;++y)for(int x=area.left;x<area.right;++x){
            BYTE* pixel=bufferPixels+(size_t(y)*width+x)*4;
            int alpha=std::max(int(pixel[0]),std::max(int(pixel[1]),int(pixel[2])));
            alpha=std::max(alpha,int(coveragePixels[(size_t(y)*width+x)*4]));
            if(drawing&&x>=board.left&&x<board.right&&y>=board.top&&y<board.bottom&&
               (padSettings.drawingSurface=="white"||alpha>0))alpha=255;
            if(blackHole.ready){double dx=x-blackHole.x,dy=y-blackHole.y;
                if(dx*dx+dy*dy<holeRadius*holeRadius)alpha=255;}
            pixel[3]=BYTE(alpha);
        }
    }
    void paint() {
        // BeginPaint temporarily hides the cursor over the invalid region.
        // This overlay presents through UpdateLayeredWindow, not the paint DC.
        PAINTSTRUCT ps={};ValidateRect(window,NULL);
        // Track our own dirty region; do not rely on a layered window paint DC.
        if(IsRectEmpty(&pendingPaint))return;
        ps.rcPaint=pendingPaint;SetRectEmpty(&pendingPaint);
        HRGN clip=CreateRectRgnIndirect(&ps.rcPaint);
        SelectClipRgn(buffer,clip);SelectClipRgn(coverageDC,clip);DeleteObject(clip);
        FillRect(buffer,&ps.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));
        FillRect(coverageDC,&ps.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));double time=now();
        if(drawing&&inkDC)BitBlt(buffer,board.left,board.top,board.right-board.left,board.bottom-board.top,inkDC,0,0,SRCCOPY);
        for(const Cluster& c:clusters) {
            if(c.railSlug){double speed=std::max(1.,std::hypot(c.vx,c.vy)),tail=75+c.railEnergy*.75;thickLine(c.x-c.vx/speed*tail,c.y-c.vy/speed*tail,c.x,c.y,fadeColor(c.color,.6),9);thickLine(c.x-c.vx/speed*tail,c.y-c.vy/speed*tail,c.x,c.y,RGB(255,255,255),3);}
            else clusterDraw(c.x,c.y,c.vx,c.vy+(c.settings.gravityEnabled?c.settings.gravity*std::max(0.,time-c.born):0),time-c.born,c.color);
        }
        for(const RailTrace& beam:railTraces){double fade=clamp(1-(time-beam.born)/(.16+beam.energy*.003),0,1);thickLine(beam.x,beam.y,beam.endX,beam.endY,fadeColor(beam.color,fade*.5),int(9+beam.energy*beam.energy*.003));thickLine(beam.x,beam.y,beam.endX,beam.endY,fadeColor(RGB(255,255,255),fade),int(2+beam.energy*beam.energy*.0008));}
        if(rail.ready||time<rail.visibleUntil)railDraw(rail,time);
        for(const auto& gun:extraRails)if(gun.ready||time<gun.visibleUntil)railDraw(gun,time);
        if(dice.visible)diceDraw();
        drawHoming(time);
        if(cross.ready) {
            COLORREF color=colorForFixed(cross.settings.palette);double power=.45+cross.charge()*.55;
            crossDraw(cross.x,cross.y,cross.settings.crossReach,color,power);
            const wchar_t* hint=cross.pulling?L"광선 충전 중 · 놓으면 폭발":L"Copilot 키를 누르면 충전";
            SetTextColor(buffer,color);TextOutW(buffer,int(cross.x+20),int(cross.y+25),hint,lstrlenW(hint));
        }
        if(pendulum.ready) {
            COLORREF color=colorForFixed(pendulum.settings.palette);
            line(pendulum.anchorX,pendulum.anchorY,pendulum.x(),pendulum.y(),fadeColor(color,.7));
            SelectObject(buffer,GetStockObject(NULL_BRUSH));SetDCPenColor(buffer,color);
            Ellipse(buffer,int(pendulum.anchorX-5),int(pendulum.anchorY-5),int(pendulum.anchorX+5),int(pendulum.anchorY+5));
            Ellipse(buffer,int(pendulum.x()-13),int(pendulum.y()-13),int(pendulum.x()+13),int(pendulum.y()+13));SelectObject(buffer,GetStockObject(DC_BRUSH));
            clusterDraw(pendulum.x(),pendulum.y(),0,0,time,color);
            if(pendulum.hanging)for(int i=1;i<=5;++i)star(pendulum.x()+pendulum.vx()*.012*i,pendulum.y()+pendulum.vy()*.012*i,1,fadeColor(color,.5));
            const wchar_t* hint=pendulum.hanging?L"놓으면 날아갑니다":L"Copilot 키를 누르면 흔들립니다";
            SetTextColor(buffer,color);TextOutW(buffer,int(pendulum.x()+20),int(pendulum.y()+20),hint,lstrlenW(hint));
        }
        if(sling.ready) {
            POINT cursor;GetCursorPos(&cursor);double x=sling.pulling?sling.pouchX:cursor.x-left,y=sling.pulling?sling.pouchY:cursor.y-top;
            COLORREF color=colorForFixed(sling.settings.palette);
            if(sling.pulling) {
                double dx=x-sling.anchorX,dy=y-sling.anchorY,length=std::max(1.,std::hypot(dx,dy)),px=-dy/length*9,py=dx/length*9;
                line(sling.anchorX+px,sling.anchorY+py,x,y,fadeColor(color,.7));line(sling.anchorX-px,sling.anchorY-py,x,y,fadeColor(color,.7));
                SelectObject(buffer,GetStockObject(NULL_BRUSH));SetDCPenColor(buffer,fadeColor(color,.6));
                Ellipse(buffer,int(sling.anchorX-6),int(sling.anchorY-6),int(sling.anchorX+6),int(sling.anchorY+6));SelectObject(buffer,GetStockObject(DC_BRUSH));
                for(int i=1;i<=7;++i) { double t=.02*i;star(x+sling.vx()*t,y+sling.vy()*t+(sling.settings.gravityEnabled?.5*sling.settings.gravity*t*t:0),1,fadeColor(color,.4)); }
                std::wstring hint=L"놓으면 발사 · "+std::to_wstring(int(std::lround(std::hypot(dx,dy)/240*100)))+L"%";
                SetTextColor(buffer,color);TextOutW(buffer,int(x+20),int(y+20),hint.c_str(),int(hint.size()));
            } else {
                const wchar_t* hint=L"Copilot 키를 누르고 당겨보세요";SetTextColor(buffer,color);TextOutW(buffer,int(x+20),int(y+20),hint,lstrlenW(hint));
            }
            clusterDraw(x,y,0,0,time,color);
        }
        if(isArmed) {
            POINT cursor;GetCursorPos(&cursor);double x=cursor.x-left,y=cursor.y-top;clusterDraw(x,y,0,0,time,RGB(185,205,255));
            const wchar_t* hint=armed.fire=="middle"?L"가운데 클릭 → 발사":armed.fire=="left"?L"클릭 → 발사":L"Space → 발사";
            SetTextColor(buffer,RGB(176,196,222));TextOutW(buffer,int(x+20),int(y+20),hint,lstrlenW(hint));
        }
        for(const PadContact& c:padContacts){if(drawing||padRail)break;double x=padBounds.left+24+c.x*(padBounds.right-padBounds.left-48),y=padBounds.top+24+c.y*(padBounds.bottom-padBounds.top-48);COLORREF color=colorForFixed(padSettings.palette);
            SelectObject(buffer,GetStockObject(NULL_BRUSH));SetDCPenColor(buffer,fadeColor(color,.55));Ellipse(buffer,int(x-12),int(y-12),int(x+12),int(y+12));SelectObject(buffer,GetStockObject(DC_BRUSH));star(x,y,8,color);star(x,y,3,RGB(255,255,255));
        }
        for(const Particle& p:particles) {
            if(p.kind==4)continue;
            if(p.x < -100||p.y < -100||p.x > width+100||p.y > height+100)continue;
            double fade=clamp((p.life-(time-p.born))/.55,0,1);
            if(p.kind==3||p.kind==4)fade=std::min(fade,clamp((time-p.born)/.3,0,1));
            if(p.kind==0) {
                double angle=p.angle*.0174532925199,co=std::cos(angle),si=std::sin(angle),hw=p.size*std::max(.15,std::abs(co))*fade/2,hh=p.size*fade/2;
                double xs[4]={-hw,hw,hw,-hw},ys[4]={-hh,-hh,hh,hh};POINT points[4];
                for(int i=0;i<4;++i)points[i]={LONG(p.x+xs[i]*co-ys[i]*si),LONG(p.y+xs[i]*si+ys[i]*co)};
                SetDCPenColor(buffer,p.color);SetDCBrushColor(buffer,p.color);Polygon(buffer,points,4);opacityPolygon(points,4);
            } else {
                COLORREF color=fadeColor(p.color,fade);
                if(p.railStyle==1){star(p.x,p.y,p.size*fade,color);}
                else if(p.railStyle==2){
                    double speed=std::max(1.,std::hypot(p.vx,p.vy)),tail=std::min(65.,speed*.045);
                    line(p.x-p.vx/speed*tail,p.y-p.vy/speed*tail,p.x,p.y,fadeColor(color,.55));
                    star(p.x,p.y,p.size*fade,color);star(p.x,p.y,1,fadeColor(RGB(255,255,255),fade));
                }else if(p.railStyle==3){
                    double r=p.size*fade;line(p.x-r,p.y,p.x,p.y-r,color);line(p.x,p.y-r,p.x+r,p.y,color);
                    line(p.x+r,p.y,p.x,p.y+r,color);line(p.x,p.y+r,p.x-r,p.y,color);
                    line(p.x-p.vx*.035,p.y-p.vy*.035,p.x,p.y,fadeColor(color,.4));
                    star(p.x,p.y,1.5,fadeColor(RGB(255,255,255),fade));
                }else {line(p.x-p.vx*.05,p.y-p.vy*.05,p.x,p.y,fadeColor(color,.33));star(p.x,p.y,p.size*fade,color);}
            }
        }
        if(blackHole.ready) {
            double x=blackHole.x,y=blackHole.y,r=blackHole.radius();COLORREF color=colorForFixed(blackHole.settings.palette);
            SelectObject(buffer,GetStockObject(NULL_BRUSH));
            for(int i=3;i>=1;--i) {
                SetDCPenColor(buffer,fadeColor(color,.18+.2*(4-i)));
                double rx=r*(1.7+i*.3),ry=r*(.3+i*.1);
                Ellipse(buffer,int(x-rx),int(y-ry),int(x+rx),int(y+ry));
            }
            SelectObject(buffer,GetStockObject(DC_BRUSH));SetDCBrushColor(buffer,RGB(1,1,4));SetDCPenColor(buffer,color);
            Ellipse(buffer,int(x-r),int(y-r),int(x+r),int(y+r));
            SetDCBrushColor(buffer,color);double angle=time*2;double rx=r*2.3,ry=r*.6;
            for(int i=0;i<3;++i){double a=angle+i*2.09439510239;star(x+std::cos(a)*rx,y+std::sin(a)*ry,2,color);}
            std::wstring hint=blackHole.pulling?L"모은 별 "+std::to_wstring(blackHole.captured)+L" · 놓으면 방출":L"Copilot 키를 누르면 흡수 시작";
            SetTextColor(buffer,color);TextOutW(buffer,int(x+r+12),int(y+r+14),hint.c_str(),int(hint.size()));
        }
        SelectObject(buffer,GetStockObject(NULL_BRUSH));
        for(const Shock& wave:shocks) {
            double age=clamp((time-wave.born)/.7,0,1),radius=15+age*210;SetDCPenColor(buffer,fadeColor(wave.color,1-age));
            Ellipse(buffer,int(wave.x-radius),int(wave.y-radius),int(wave.x+radius),int(wave.y+radius));
        }
        SelectObject(buffer,GetStockObject(DC_BRUSH));
        if(time-cross.flashAt<.18) {double fade=1-(time-cross.flashAt)/.18;crossDraw(cross.x,cross.y,cross.settings.crossReach*(1+(1-fade)*.5),RGB(255,255,255),fade);}
        finalizeAlpha(&ps.rcPaint);
        SelectClipRgn(buffer,NULL);SelectClipRgn(coverageDC,NULL);
        POINT destination={left,top},origin={0,0};SIZE dimensions={width,height};
        BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(window,NULL,&destination,&dimensions,buffer,&origin,0,&blend,ULW_ALPHA))
            log("AlphaPresentError="+std::to_string(GetLastError()));
        SelectClipRgn(buffer,NULL);
        if(!firstPaint && (homingReady||!guidedShots.empty()||!particles.empty()||!clusters.empty()||isArmed||sling.ready||pendulum.ready||blackHole.ready||cross.ready||rail.ready||dice.visible||drawing)) { firstPaint=true;log("FirstPaint"); }
        if(!snapshotPath.empty()&&!snapshotSaved&&time-started>.35) {
            snapshotSaved=true;BITMAPINFO info={};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            std::vector<BYTE> pixels(size_t(width)*height*4);HGDIOBJ selected=SelectObject(buffer,oldBitmap);HDC screen=GetDC(NULL);
            int rows=GetDIBits(screen,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS);ReleaseDC(NULL,screen);SelectObject(buffer,selected);
            if(rows==height) {
                BITMAPFILEHEADER header={};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+DWORD(pixels.size());
                HANDLE f=CreateFileW(snapshotPath.c_str(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
                if(f!=INVALID_HANDLE_VALUE) { DWORD written;WriteFile(f,&header,sizeof(header),&written,NULL);WriteFile(f,&info.bmiHeader,sizeof(BITMAPINFOHEADER),&written,NULL);WriteFile(f,pixels.data(),DWORD(pixels.size()),&written,NULL);CloseHandle(f);log("SnapshotSaved"); }
            }
        }
    }
};
static Engine* activeEngine = NULL;
static LRESULT CALLBACK copilotHook(int code,WPARAM message,LPARAM payload) {
    if(code==HC_ACTION&&activeEngine&&!activeEngine->closing) {
        const KBDLLHOOKSTRUCT* key=reinterpret_cast<const KBDLLHOOKSTRUCT*>(payload);
        if(key->vkCode==VK_F12){
            bool up=message==WM_KEYUP||message==WM_SYSKEYUP;
            if(up&&activeEngine->f12Captured){activeEngine->f12Captured=false;return 1;}
            if(!up&&(activeEngine->railInputHeld||activeEngine->copilotHold.held)){
                if(!activeEngine->f12Captured){activeEngine->f12Captured=true;
                    if(activeEngine->window)PostMessageW(activeEngine->window,SettingsMessage,0,0);
                    else activeEngine->settingsPending=true;}
                return 1;
            }
        }
        if(key->vkCode==VK_F23&&(acceptTestInput?(key->flags&LLKHF_INJECTED)!=0:(key->flags&LLKHF_INJECTED)==0)) {
            bool up=message==WM_KEYUP||message==WM_SYSKEYUP;
            activeEngine->railInputHeld=!up;
            POINT position;GetCursorPos(&position);
            if(activeEngine->window) {
                CopilotEvent* event=new(std::nothrow) CopilotEvent{up,key->time,position};
                if(event&&!PostMessageW(activeEngine->window,CopilotMessage,0,reinterpret_cast<LPARAM>(event)))delete event;
            } else activeEngine->startupEvents.push_back({up,key->time,position});
            return 1; // Engine owns Copilot input; stop PowerToys launching on repeats.
        }
    }
    return CallNextHookEx(NULL,code,message,payload);
}
static LRESULT CALLBACK railMouseHook(int code,WPARAM message,LPARAM payload) {
    if(code==HC_ACTION&&activeEngine){
        const MSLLHOOKSTRUCT* mouse=reinterpret_cast<const MSLLHOOKSTRUCT*>(payload);
        if(message==WM_MBUTTONUP&&activeEngine->middleCaptured){activeEngine->middleCaptured=false;return 1;}
        if(message==WM_MBUTTONDOWN&&activeEngine->blocksWheel()){
            activeEngine->middleCaptured=true;
            if(activeEngine->window)PostMessageW(activeEngine->window,RailPlaceMessage,WPARAM(INT_PTR(mouse->pt.x)),LPARAM(INT_PTR(mouse->pt.y)));
            return 1;
        }
    }
    if(code==HC_ACTION&&activeEngine&&(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL)&&activeEngine->blocksWheel()) {
        const MSLLHOOKSTRUCT* mouse=reinterpret_cast<const MSLLHOOKSTRUCT*>(payload);int delta=SHORT(HIWORD(mouse->mouseData));
        if(activeEngine->window)PostMessageW(activeEngine->window,RailWheelMessage,WPARAM(INT_PTR(delta)),0);
        else activeEngine->startupEvents.push_back({false,mouse->time,mouse->pt,delta,true});
        return 1;
    }
    return CallNextHookEx(NULL,code,message,payload);
}
static LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    Engine* engine=reinterpret_cast<Engine*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) { auto create=reinterpret_cast<CREATESTRUCTW*>(l);engine=reinterpret_cast<Engine*>(create->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(engine));engine->window=window; }
    if(!engine)return DefWindowProcW(window,message,w,l);
    switch(message) {
        case SettingsMessage:engine->requestSettings();return 0;
        case WM_INPUT:engine->padInput(l);return DefWindowProcW(window,message,w,l);
        case RailPlaceMessage:{POINT point={LONG(INT_PTR(w)),LONG(INT_PTR(l))};engine->placeRail(point);return 0;}
        case RailWheelMessage:engine->chargeRail(int(INT_PTR(w)));return 0;
        case CopilotMessage: {
            CopilotEvent* event=reinterpret_cast<CopilotEvent*>(l);
            engine->copilot(event->up,event->time,&event->position);delete event;return 0;
        }
        case WM_COPYDATA: {
            auto data=reinterpret_cast<COPYDATASTRUCT*>(l);
            if(engine->closing||data->cbData<1||data->cbData>16384)return 0;
            if(data->dwData==2) { log("LaunchIgnored=AlreadyRunning");return 1; }
            if(data->dwData!=1)return 0;
            try { Settings settings=Settings::from(std::string(static_cast<const char*>(data->lpData),data->cbData));engine->trigger(settings);UpdateWindow(window);return 1; }
            catch(const std::exception&) { return 0; }
        }
        case WM_TIMER:engine->tick();return 0;
        case WM_PAINT:engine->paint();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_NCHITTEST:return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_SETCURSOR:return TRUE; // Preserve the underlying application cursor.
        case WM_CLOSE:engine->finish("Close");return 0;
        case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
static void require(bool condition,const char* why) { if(!condition)throw std::runtime_error(why); }
static void selfTest() {
    {Engine test;test.homingReady=true;test.homingHolding=true;test.homingX=500;test.homingY=500;
        test.releaseHoming(900,500,0);require(test.guidedShots.size()==30,"Homing 10 groups of 3 shots");
        for(const auto& m:test.guidedShots)require(m.vx<0,"Homing launches backwards");
        test.stepHoming(.02,.02);require(test.guidedShots[0].x<500&&test.guidedShots[29].x==500,"Homing delayed groups");
        for(int i=2;i<=1800;++i)test.stepHoming(i/240.,1./240.);
        require(test.guidedShots.empty()&&test.explosions==30,"Homing returns and explodes all shots");
        test.homingReady=true;test.homingHolding=true;test.releaseHoming(500,500,10);
        require(std::isfinite(test.guidedShots[0].nx),"Homing same-position target is safe");
        test.finish("SelfTest");require(test.guidedShots.empty()&&!test.homingReady,"Homing cleanup");}

    {Engine test;Cluster c={};c.x=300;c.y=300;c.settings.clusterCount=40;c.railEnergy=10;
        std::vector<Particle> low,high;test.railImpact(low,c,1,false);c.railEnergy=100;test.railImpact(high,c,1,false);
        require(low.size()==40&&high.size()==40,"Rail impact honors particle count");
        for(const Particle& p:low)require(p.railStyle==1&&std::hypot(p.vx,p.vy)<500,"Low charge compact star burst");
        bool cores=false;for(const Particle& p:high){require(std::hypot(p.vx,p.vy)>=1080&&p.life>=1.95,"Full charge fast long-lived burst");cores|=p.railStyle==3;}
        require(cores&&test.shocks.size()==1,"Full charge adds diamond sparks and shockwave");}

    {Engine glow;glow.setupBuffer();glow.rail.x=250;glow.rail.y=250;
        glow.railChargeGlow(glow.rail,1,1,RGB(100,180,255));glow.finalizeAlpha();
        BYTE* outside=glow.bufferPixels+(size_t(192)*glow.width+350)*4;
        require(outside[3]>0&&outside[3]<100,"Rail charging halo is translucent");
        require(glow.coveragePixels[(size_t(192)*glow.width+350)*4]==0,"Charging halo has no opaque coverage");}

    {Engine render;render.setupBuffer();render.rail.ready=true;render.rail.x=250;render.rail.y=250;
        render.railDraw(render.rail,now());render.finalizeAlpha();
        BYTE* body=render.bufferPixels+(size_t(250)*render.width+225)*4;
        require(body[3]==255,"Dark rail body is opaque");
        BYTE* empty=render.bufferPixels;require(empty[3]==0,"Empty background is transparent");
        render.star(500,500,3,RGB(30,60,90));render.finalizeAlpha();
        BYTE* glow=render.bufferPixels+(size_t(500)*render.width+500)*4;
        require(glow[3]==90,"Dim light remains translucent");}

    {Engine two;two.padRail=true;two.rail.ready=true;two.rail.holding=true;two.rail.settings.railCharge=10;two.extraRails.push_back(two.rail);
    two.railPadInput({{1,.2,.2},{2,.6,.6}});require(two.rail.energy==0,"Two-finger landing does not charge");
    two.railPadInput({{2,.635,.6},{1,.235,.2}});require(std::abs(two.rail.energy-10)<.1&&two.extraRails[0].energy==two.rail.energy,"Two-finger motion charges all railguns with reordered IDs");
    double energy=two.rail.energy;two.railPadInput({{1,.235,.2},{2,.635,.6}});require(two.rail.energy==energy,"Stationary fingers do not charge");
    two.railPadInput({{1,.4,.2}});two.railPadInput({{1,.8,.2},{2,.9,.6}});require(two.rail.energy==energy,"Finger count change does not jump charge");
    two.rail.holding=false;two.railPadInput({{1,.9,.2},{2,1,.6}});require(two.rail.energy==energy,"Released railgun ignores touch charge");}

    {Engine draw;draw.padSettings.drawingSurface="white";draw.setupBuffer();draw.openBoard({0,0,1000,700});draw.padReady=true;
    draw.drawContacts({{0,.2,.2}});POINT firstInk=draw.inkLast;draw.drawContacts({{0,.8,.8}});
    require(draw.penDown&&GetPixel(draw.inkDC,firstInk.x,firstInk.y)==RGB(38,65,118),"Drawing ink persists on canvas");
    draw.drawContacts({});require(!draw.penDown,"Drawing lift ends stroke");
    draw.drawContacts({{0,.3,.3},{1,.5,.5}});require(draw.penDown&&draw.inkHeads.size()==2,"Drawing multiple fingers independently");
    draw.padStep(100,.016);require(draw.padReady&&draw.drawing,"Drawing does not idle exit");
    draw.copilotHold.held=true;draw.copilot(false,0);require(!draw.closing,"Drawing repeat does not toggle");
    draw.copilot(true,0);require(!draw.closing,"Drawing release keeps canvas");
    draw.drawOpened=-1;draw.copilot(false,0);require(draw.closing&&!draw.drawing,"Drawing second press closes");}

    {Engine overlay;overlay.setupBuffer();overlay.openBoard({0,0,1000,700});require(GetPixel(overlay.inkDC,0,0)==RGB(0,0,0),"Screen drawing transparent background");overlay.drawContacts({{0,.2,.2},{1,.8,.8}});require(overlay.inkHeads.size()==2,"Screen drawing multitouch");}
    {BYTE report[30]={1,3,0xFB,5,0xBC,3};std::vector<PadContact> contacts;
    require(parsePad(report,30,contacts)&&contacts.size()==1&&contacts[0].x==1&&contacts[0].y==1,"Touchpad coordinate normalization");
    report[6]=7;report[7]=30;require(parsePad(report,30,contacts)&&contacts.size()==2,"Touchpad multiple fingers");
    report[1]=0;report[6]=0;require(parsePad(report,30,contacts)&&contacts.empty(),"Touchpad release");
    require(!parsePad(report,29,contacts),"Touchpad short report rejected");report[1]=3;report[3]=255;require(parsePad(report,30,contacts)&&contacts.empty(),"Touchpad invalid contact ignored");
    Engine pad;pad.padReady=true;pad.padUntil=2;pad.padLastReport=0;pad.padContacts.push_back({0,.5,.5});pad.padBounds={0,0,1000,700};pad.padStep(.016,.016);require(!pad.particles.empty(),"Touchpad emits stardust");pad.padStep(.5,.016);require(pad.padContacts.empty(),"Touchpad stale contact cleared");pad.padStep(3,.016);require(!pad.padReady,"Touchpad idle shutdown");}

    {
        Engine orbit;orbit.blackHole.x=500;orbit.blackHole.y=500;orbit.blackHole.settings.blackHoleOrbitDamping=0;
        Particle star;star.kind=3;star.x=800;star.y=500;star.vy=orbit.blackHole.circularSpeed(300);
        double initialMomentum=300*star.vy,initialEnergy=.5*star.vy*star.vy-orbit.blackHole.mu()/std::sqrt(90000+64.);
        for(int i=0;i<2400;++i)require(!orbit.stepOrbit(star,1./120.),"Circular orbit does not collapse");
        double radius=std::hypot(star.x-500,star.y-500),momentum=(star.x-500)*star.vy-(star.y-500)*star.vx;
        double energy=.5*(star.vx*star.vx+star.vy*star.vy)-orbit.blackHole.mu()/std::sqrt(radius*radius+64.);
        require(std::abs(radius/300-1)<.01&&std::abs(momentum/initialMomentum-1)<.001&&std::abs(energy/initialEnergy-1)<.002,"Orbit radius, angular momentum and energy conserved");
    }
    {
        Settings s=Settings::from("{\"Effect\":\"railgun\",\"RailChargePerNotch\":8,\"RailPower\":1}");
        require(s.effect=="railgun","Rail settings");
        Engine test;test.copilotHold.held=true;test.railInputHeld=true;POINT point={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
        test.trigger(s,&point);double x=test.rail.x,y=test.rail.y;
        DiceState die;die.visible=true;die.holding=true;die.step(120,80,.016,1);require(!die.settled,"Dice shakes on cursor motion");
        for(int i=0;i<120;++i)die.step(120,80,.016,1+i*.016);
        require(die.settled,"Dice settles after stopping");
        for(double angle:die.angle)require(std::abs(angle/1.5707963267948966-std::round(angle/1.5707963267948966))<.001,"Dice face is square to screen");
        require(test.rail.ready&&test.blocksWheel(),"Rail wheel capture while held");
        test.rail.aim(x+100,y+100);require(test.rail.x==x&&test.rail.y==y&&std::abs(test.rail.dx-test.rail.dy)<1e-8,"Rail fixed installation and aim");
        test.chargeRail(120);test.chargeRail(-120);test.chargeRail(60);require(test.rail.energy==20,"Bidirectional and partial wheel charge");
        test.chargeRail(12000);require(test.rail.energy==100,"Rail charge limit");
        POINT placed={point.x+100,point.y+100};test.placeRail(placed);require(test.extraRails.size()==1,"Additional railgun placement");
        test.extraRails.clear();
        test.releaseRail(x+300,y,now());require(test.clusters.size()==1&&test.clusters[0].railSlug&&test.railTraces.size()==1&&!test.blocksWheel(),"Rail release shot and unblock");
        test.releaseRail(x+300,y,now());require(test.clusters.size()==1,"Rail release fires once");
        test.finish("SelfTest");require(!test.blocksWheel()&&test.railTraces.empty(),"Rail cancel cleanup");
        Engine empty;empty.copilotHold.held=true;empty.trigger(s,&point);empty.releaseRail(x+300,y,now());require(empty.clusters.empty(),"Uncharged railgun does not fire");
    }
    {
        PendulumState elastic;elastic.angle=0;elastic.settings.pendulumElastic=true;elastic.settings.pendulumStiffness=60;
        for(int i=0;i<1000;++i)elastic.step(.004);
        require(std::abs(elastic.length-(180+1000./60))<.2,"Elastic line extends under weight");
        elastic.settings.pendulumGravity=0;elastic.omega=0;
        for(int i=0;i<1000;++i)elastic.step(.004);
        require(std::abs(elastic.length-elastic.restLength)<.2,"Elastic line recovers when force removed");
        elastic.length=180;elastic.omega=12;elastic.settings.pendulumGravity=1000;
        for(int i=0;i<100;++i)elastic.step(.004);
        require(elastic.length>200&&elastic.length<=elastic.restLength*3,"Centrifugal force stretches line within limit");
        elastic.angle=0;elastic.omega=0;elastic.anchorVX=100;elastic.anchorVY=-50;elastic.radialSpeed=200;
        require(elastic.vx()==100&&elastic.vy()==150,"Elastic radial velocity included on release");
        PendulumState fixed;fixed.angle=0;fixed.omega=12;for(int i=0;i<100;++i)fixed.step(.004);
        require(fixed.length==180&&fixed.radialSpeed==0,"Elastic disabled keeps fixed length");
    }
    {
        PendulumState moving;moving.anchorX=500;moving.anchorY=500;moving.angle=0;
        moving.follow(600,500,.1);require(moving.anchorX==600&&moving.anchorY==500&&moving.angle<0&&moving.anchorVX>0,"Moving cursor pivot accelerates pendulum");
        moving.angle=0;moving.omega=2;moving.anchorVX=100;moving.anchorVY=-50;
        require(moving.vx()==460&&moving.vy()==-50,"Release combines pivot and relative velocity");
        PendulumState shake;shake.angle=0;shake.settings.pendulumDamping=0;
        for(int i=1;i<=400;++i){double time=i*.01;shake.follow(150*std::sin(time*2.35),0,.01);}
        require(std::abs(shake.omega)>.1,"Mouse shaking supplies pendulum energy");
        require(std::isfinite(shake.angle)&&std::isfinite(shake.vx()),"Driven pendulum remains finite");
    }
    {
        Settings s=Settings::from("{\"Effect\":\"crossflash\",\"CrossGatherRate\":120,\"CrossReach\":450,\"CrossBurstCount\":50}");
        require(s.effect=="crossflash"&&s.crossBurst==50,"Cross flash settings");
        Engine test;test.copilotHold.held=true;POINT point={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
        test.trigger(s,&point);require(test.cross.ready&&test.cross.pulling&&test.particles.empty(),"Cross begins gathering without burst");
        double time=now();test.feedCross(.1,time);require(test.particles.size()==12,"Cross gather rate");
        int arms[4]={};for(const Particle& p:test.particles){++arms[p.arm];require((p.arm<2&&p.y==test.cross.y)||(p.arm>=2&&p.x==test.cross.x),"Cross particles remain on axes");}
        for(int count:arms)require(count==3,"Cross lights balance four arms");
        test.copilot(false,0);require(test.triggers==1,"Repeat does not reset cross");
        for(int i=1;i<=20;++i)test.physics(time+i*.04,.04,0,0);
        require(test.cross.collected==12&&test.particles.empty()&&test.cross.ready,"Cross lights gather into center");
        test.releaseCross(test.cross.x,test.cross.y,time+1);require(!test.cross.ready&&test.particles.size()==62&&test.shocks.size()==1,"Cross releases one explosion");
        test.releaseCross(0,0,time+1);require(test.particles.size()==62,"Duplicate cross release ignored");
        test.finish("SelfTest");require(!test.cross.ready&&test.cross.flashAt==-100,"Cross cleanup");
    }
    BlackHoleState inverseGrowth;inverseGrowth.settings.blackHoleGrowth=1;inverseGrowth.captured=10;double r10=inverseGrowth.radius();
    inverseGrowth.captured=11;double r11=inverseGrowth.radius();
    require(std::abs((std::pow(r11,4)-std::pow(r10,4))/BlackHoleState::GrowthFourthPerStar-1)<1e-8,"Inverse cube integrated growth law");
    inverseGrowth.captured=400;double r400=inverseGrowth.radius();
    inverseGrowth.captured=401;double r401=inverseGrowth.radius();
    require(r11-r10>r401-r400&&std::abs(r400-173.21318734282121)<1e-8,"Growth slows with radius and keeps calibration");
    BlackHoleState growth;double baseAcceleration=growth.acceleration(500);
    growth.captured=400;require(growth.radius()>70&&growth.radius()<110&&growth.acceleration(500)>baseAcceleration*1.5,"Slower black hole growth and mass-based gravity");
    growth.captured=100000;require(growth.radius()==320&&growth.acceleration(500)>baseAcceleration*40,"Large black hole cap");
    growth.settings.blackHoleStrength=5;require(std::isfinite(growth.acceleration(1))&&growth.acceleration(0)==0,"Softened gravity remains finite");
    require(std::abs(growth.acceleration(10000)/growth.acceleration(20000)-4)<.001,"Far field inverse square gravity");
    Settings toys=Settings::from("{\"Effect\":\"pendulum\",\"PendulumLength\":180,\"PendulumGravity\":1000,\"PendulumDamping\":0,\"BlackHoleCount\":30,\"BlackHoleBurstCount\":50}");
    require(toys.effect=="pendulum"&&toys.pendulumDamping==0&&toys.blackHoleCount==30,"Toy settings");
    PendulumState pend;pend.length=180;pend.angle=1.0471975512;pend.settings=toys;
    double initialEnergy=.5*pend.length*pend.length*pend.omega*pend.omega+pend.settings.pendulumGravity*pend.length*(1-std::cos(pend.angle));
    for(int i=0;i<1000;++i)pend.step(.001);
    double energy=.5*pend.length*pend.length*pend.omega*pend.omega+pend.settings.pendulumGravity*pend.length*(1-std::cos(pend.angle));
    require(std::abs(energy/initialEnergy-1)<.01&&std::abs(pend.omega)>.1,"Pendulum oscillation and energy");
    require(std::abs((pend.x()-pend.anchorX)*pend.vx()+(pend.y()-pend.anchorY)*pend.vy())<1e-6,"Pendulum tangential velocity");
    {
        Engine test;test.copilotHold.held=true;POINT point={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
        test.trigger(toys,&point);require(test.pendulum.hanging&&test.clusters.empty(),"Pendulum hangs without firing");
        require(test.pendulum.angle==0&&test.pendulum.omega==0&&test.pendulum.x()==test.pendulum.anchorX,"Pendulum always starts vertically at zero");
        test.pendulum.step(.25);double time=now();test.pendulum.last=time;double x=test.pendulum.x(),y=test.pendulum.y(),vx=test.pendulum.vx(),vy=test.pendulum.vy();
        test.releasePendulum(time);require(test.clusters.size()==1&&!test.pendulum.ready,"Pendulum release once");
        require(std::abs(test.clusters[0].x-x)<1e-8&&std::abs(test.clusters[0].y-y)<1e-8&&std::abs(test.clusters[0].vx-vx)<1e-8&&std::abs(test.clusters[0].vy-vy)<1e-8,"Pendulum momentum transfer");
        test.releasePendulum(time);require(test.clusters.size()==1,"Pendulum duplicate release ignored");
    }
    {
        Engine test;test.copilotHold.held=true;toys.effect="blackhole";POINT point={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
        test.trigger(toys,&point);require(test.blackHole.pulling&&test.particles.empty(),"Initial stars are spread over time");
        double feedTime=test.blackHole.lastFeed;test.blackHole.settings.blackHoleFeed=0;
        test.feedBlackHole(feedTime+.1);require(test.particles.size()==3,"Initial generation does not arrive in a single batch");
        for(int i=2;i<=8;++i)test.feedBlackHole(feedTime+i*.1);
        require(test.particles.size()==30,"Initial count preserved over time");
        test.feedBlackHole(feedTime+.9);require(test.particles.size()==30,"Zero continuous generation");
        test.blackHole.settings.blackHoleFeed=75;
        for(int i=1;i<=7;++i)test.feedBlackHole(feedTime+.9+i*.05);
        require(test.particles.size()==105,"Continuous count spread over 0.35 seconds");
        double minX=1e9,maxX=-1e9,minY=1e9,maxY=-1e9;
        for(const Particle& p:test.particles){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);require(std::hypot(p.x-test.blackHole.x,p.y-test.blackHole.y)>test.blackHole.radius()+24,"Spawn avoids black hole");}
        require(maxX-minX>test.width*.6&&maxY-minY>test.height*.6,"Spawn distributed over screen");
        test.particles.clear();Particle star;star.kind=3;star.x=test.blackHole.x+1;star.y=test.blackHole.y;star.life=100;star.born=now();test.particles.push_back(star);
        double time=now();test.physics(time,.001,0,0);require(test.blackHole.captured==1&&test.particles.empty()&&test.blackHole.radius()>16,"Black hole capture and growth");
        test.releaseBlackHole(test.blackHole.x,test.blackHole.y,time);require(!test.blackHole.ready&&test.particles.size()==1&&test.shocks.size()==1,"One captured star means one released star");
        for(const Particle& p:test.particles)require(p.kind==2&&p.confined&&p.fall==0&&std::hypot(p.vx,p.vy)>=249.99&&std::hypot(p.vx,p.vy)<=3600.01&&p.life>=1.4&&p.life<=3.8,"Varied release speed and lifetime");
        test.releaseBlackHole(0,0,time);require(test.shocks.size()==1,"Black hole duplicate release ignored");
        test.physics(time+4,.04,0,0);require(test.particles.empty(),"Black hole release particles expire");
        test.finish("SelfTest");require(test.shocks.empty()&&!test.blackHole.ready&&!test.pendulum.ready,"Toy cleanup");
    }
    {
        Engine test;test.blackHole.ready=true;test.blackHole.pulling=true;test.blackHole.settings=toys;double time=now();
        test.releaseBlackHole(500,500,time);require(test.particles.empty(),"Zero captured stars means zero new stars");
        test.blackHole.ready=true;test.blackHole.pulling=true;test.blackHole.captured=37;
        test.releaseBlackHole(500,500,time);require(test.particles.size()==37,"Exact captured count");
        double minSpeed=1e9,maxSpeed=0,minX=1e9,maxX=-1e9;
        for(const Particle& p:test.particles){double speed=std::hypot(p.vx,p.vy);minSpeed=std::min(minSpeed,speed);maxSpeed=std::max(maxSpeed,speed);minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);}
        require(maxSpeed-minSpeed>1000&&maxX-minX>10,"Burst has broad velocity and position distribution");
        test.particles.clear();test.blackHole.ready=true;test.blackHole.pulling=true;test.blackHole.captured=15000;
        test.releaseBlackHole(500,500,time);require(test.particles.size()==12000&&test.collectedBursts.size()==1&&test.collectedBursts[0].remaining==3000,"Large count retained in emission queue");
        test.physics(time+4,.04,0,0);test.emitCollectedBursts(time+4);
        require(test.particles.size()==3000&&test.collectedBursts.empty(),"All captured stars emitted without truncation");
    }
    HoldState hold;
    require(hold.update(false)&&hold.held,"Initial down");
    for(int i=0;i<100;++i)require(!hold.update(false)&&hold.held,"Repeated down must not fire");
    require(hold.repeats==100,"Repeat count");
    require(!hold.update(true)&&!hold.held,"Up must end hold");
    require(hold.update(false),"Second press must fire");
    HoldState bootstrap;bootstrap.held=true;
    require(!bootstrap.update(false),"Startup repeat must not double-fire");
    bootstrap.update(true);require(bootstrap.update(false),"Press after startup release");
    SlingState pull;pull.anchorX=pull.anchorY=500;pull.bounds={0,0,1000,1000};pull.settings.slingPower=6;
    pull.pull(600,500);require(pull.pouchX==600&&pull.vx()==-600&&pull.vy()==0,"Slingshot reverse direction");
    pull.pull(500,400);require(pull.vx()==0&&pull.vy()==600,"Slingshot vertical direction");
    pull.pull(1500,1500);require(std::abs(std::hypot(pull.pouchX-500,pull.pouchY-500)-240)<1e-6,"Slingshot pull cap");
    pull.pull(500,500);require(pull.vx()==0&&pull.vy()==0,"Slingshot zero pull");
    {
        Engine test;Settings slingSettings;slingSettings.fire="slingshot";slingSettings.speed=4;slingSettings.slingPower=6;
        POINT origin={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};test.copilotHold.held=true;
        test.trigger(slingSettings,&origin);
        require(test.sling.ready&&test.sling.pulling&&test.clusters.empty(),"Slingshot prepares without firing");
        double x=test.sling.anchorX,y=test.sling.anchorY;
        for(int i=0;i<10;++i)test.copilot(false,0);
        require(test.triggers==1&&test.clusters.empty(),"Slingshot repeats do not reset or fire");
        test.releaseSling(x+100,y,now());require(!test.sling.ready&&test.clusters.size()==1,"Slingshot release once");
        require(test.clusters[0].x==x+100&&test.clusters[0].vx==-600,"Slingshot launch point and independent power");
        test.releaseSling(x+100,y,now());require(test.clusters.size()==1,"Slingshot duplicate release ignored");
        test.trigger(slingSettings,&origin);test.finish("Escape");
        require(test.cancelUntilUp&&!test.sling.ready&&test.clusters.empty(),"Escape clears held slingshot");
        test.copilot(true,0);require(test.closing,"Escape exits on release without relaunch");
    }
    Settings defaults=Settings::from("{}");require(defaults.clusterCount==150,"Default settings");
    Settings gravitySettings=Settings::from("{\"ClusterGravityEnabled\":true,\"ClusterGravity\":1000}");
    require(gravitySettings.gravityEnabled&&gravitySettings.gravity==1000,"Gravity settings");
    {
        Cluster falling={500,500,0,0,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)};
        trajectory(falling);require(std::abs(falling.edgeTime-1)<1e-8&&falling.edgeSides==8&&falling.edgeY==1000,"Falling bottom contact");
        Cluster upward={500,500,0,-2000,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)};
        trajectory(upward);require(std::abs(upward.edgeTime-(2-std::sqrt(3.)))<1e-8&&upward.edgeSides==4,"Upward first top contact");
        Cluster curved={500,500,1000,0,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)};
        trajectory(curved);require(curved.edgeX==1000&&std::abs(curved.edgeY-625)<1e-8,"Curved side contact");
        Cluster cornerGravity={500,500,500,0,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)};
        trajectory(cornerGravity);require(cornerGravity.edgeSides==(2|8)&&cornerGravity.edgeX==1000&&cornerGravity.edgeY==1000,"Curved corner contact");
        Cluster tangent={500,500,0,-1000,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)};
        trajectory(tangent);require(std::abs(tangent.edgeTime-1)<1e-8&&tangent.edgeSides==4,"Tangent top contact");
        Engine test;gravitySettings.fuse=2;
        test.clusters.push_back({500,500,100,0,0,{0,0,1000,1000},gravitySettings,RGB(255,255,255)});
        test.physics(.5,.01,0,0);require(test.clusters.size()==1&&test.clusters[0].x==550&&test.clusters[0].y==625,"Analytic gravity position");
        test.physics(3,.04,0,0);require(test.clusters.empty()&&test.particles.size()==150,"Late frame boundary burst");
        for(const Particle& particle:test.particles)require(std::abs(particle.x-600)<1e-8&&particle.y==1000,"Curved explosion stays on boundary");
    }
    Settings s=Settings::from("{\"Effect\":\"gravity\",\"ClusterCount\":99999,\"GravityStrength\":0.4,\"Palette\":\"\\u0069ce\"}");
    require(s.effect=="gravity"&&s.clusterCount==1000&&s.palette=="ice"&&std::abs(s.strength-.4)<.001,"Settings decoding");
    bool rejected=false;try { Settings::from("{\"ClusterCount\":oops}"); }catch(...) { rejected=true; }require(rejected,"Malformed settings");
    double vx[5]={10000,-10000,0,0,10000},vy[5]={0,0,10000,-10000,5000};
    for(int i=0;i<5;++i) { Cluster c={500,500,vx[i],vy[i],0,{0,0,1000,1000},defaults,RGB(255,255,255)};require(move(c,.1)&&c.x>=0&&c.x<=1000&&c.y>=0&&c.y<=1000,"Edge collision");if(i==4)require(c.x==1000&&c.y==750,"First edge contact"); }
    double hx,hy;require(collision(-80,0,80,0,0,0,16,hx,hy),"Swept cursor collision");
    Engine engine;Particle p;p.kind=1;p.x=-80;p.vx=10000;p.radius=16;p.life=10;engine.particles.push_back(p);engine.physics(.1,.016,0,0);
    require(engine.collisions==1&&engine.particles.size()==9,"Gravity burst");engine.physics(20,.016,0,0);require(engine.particles.empty(),"Particle expiry");
    Cluster c={990,500,1000,0,0,{0,0,1000,1000},defaults,RGB(255,255,255)};engine.clusters.push_back(c);engine.physics(.02,.02,0,0);require(engine.explosions==1&&engine.particles.size()==150,"Edge explosion");engine.physics(20,.01,0,0);
    c={500,500,100,0,0,{0,0,1000,1000},defaults,RGB(255,255,255)};engine.clusters.push_back(c);engine.physics(.5,.1,0,0);require(engine.clusters.size()==1&&engine.clusters[0].x==550,"Cluster movement");engine.physics(1,.01,0,0);require(engine.clusters.empty()&&engine.particles.size()==150,"Fuse explosion");
    for(int i=0;i<5;++i) {
        Engine test;Cluster fast={500,500,vx[i]*100,vy[i]*100,0,{0,0,1000,1000},defaults,RGB(255,255,255)};
        trajectory(fast);require(std::abs(fast.edgeTime-.0005)<1e-9,"Analytic edge time");
        if(i==4)require(fast.edgeX==1000&&fast.edgeY==750,"Analytic diagonal contact");
        test.clusters.push_back(fast);test.physics(.8,.04,0,0);
        require(test.clusters.empty()&&test.particles.size()==150,"High speed edge explosion");
        for(const Particle& spark:test.particles) {
            require(spark.x==fast.edgeX&&spark.y==fast.edgeY,"Explosion location");
            if(fast.edgeSides&1)require(spark.vx>=0,"Left inward burst");if(fast.edgeSides&2)require(spark.vx<=0,"Right inward burst");
            if(fast.edgeSides&4)require(spark.vy>=0,"Top inward burst");if(fast.edgeSides&8)require(spark.vy<=0,"Bottom inward burst");
        }
        for(int frame=1;frame<=20;++frame) {
            test.physics(.8+frame*.04,.04,0,0);
            for(const Particle& spark:test.particles)require(spark.x>=0&&spark.x<=1000&&spark.y>=0&&spark.y<=1000,"Burst remains inside");
        }
    }
    Cluster corner={500,500,1000000,1000000,0,{0,0,1000,1000},defaults,RGB(255,255,255)};
    trajectory(corner);require(corner.edgeSides==(2|8)&&corner.edgeX==1000&&corner.edgeY==1000,"Corner collision");
    for(const std::string mode: {"left","middle","space"}) {
        engine.clusters.clear();engine.armed=defaults;engine.armed.fire=mode;engine.isArmed=true;engine.armedUntil=20;engine.leftDown=engine.middleDown=engine.spaceDown=false;
        bool left=mode=="left",middle=mode=="middle",space=mode=="space";
        engine.input(left,middle,space,100,100,1);engine.input(left,middle,space,100,100,1.1);require(engine.clusters.size()==1,"Held input");
        engine.input(false,false,false,100,100,1.2);engine.input(left,middle,space,100,100,1.3);require(engine.clusters.size()==2,"Repeated input");engine.input(false,false,false,100,100,10);require(!engine.isArmed,"Idle expiry");
    }
    engine.finish("SelfTest");require(engine.particles.empty()&&engine.clusters.empty()&&engine.pending.empty()&&!engine.isArmed,"Full cleanup");log("SelfTest=PASS");
}
static std::vector<std::wstring> arguments() {
    std::vector<std::wstring> result;std::wstring token;bool quoted=false,started=false;
    for(const wchar_t* p=GetCommandLineW();; ++p) {
        wchar_t c=*p;if(c==L'"') { quoted=!quoted;started=true; }
        else if(c==0||(!quoted&&(c==L' '||c==L'\t'))) { if(started) { result.push_back(token);token.clear();started=false; }if(c==0)break; }
        else { token+=c;started=true; }
    }
    return result;
}
static std::string base64(const std::wstring& text) {
    std::string out;unsigned value=0;int bits=0;
    for(wchar_t c:text) {
        if(c==L'=')break;int n=c>=L'A'&&c<=L'Z'?c-L'A':c>=L'a'&&c<=L'z'?c-L'a'+26:c>=L'0'&&c<=L'9'?c-L'0'+52:c==L'+'?62:c==L'/'?63:-1;
        if(n<0)throw std::runtime_error("Invalid preview payload");value=(value<<6)|n;bits+=6;if(bits>=8) { bits-=8;out+=char((value>>bits)&255); }
        if(out.size()>16384)throw std::runtime_error("Preview settings too large");
    }
    return out;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    // Finish Windows launch feedback even on the already-running early-exit path.
    // Posting first makes GetMessage nonblocking; no application window exists yet.
    MSG startupMessage={};
    PeekMessageW(&startupMessage,NULL,0,0,PM_NOREMOVE);
    PostThreadMessageW(GetCurrentThreadId(),WM_NULL,0,0);
    GetMessageW(&startupMessage,NULL,0,0);

    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&epoch);
    HANDLE mutex=NULL;bool owner=false;
    try {
        auto dpi=reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));if(dpi)dpi(reinterpret_cast<HANDLE>(-4));
        POINT bootCursor;GetCursorPos(&bootCursor);
        wchar_t path[32768];DWORD n=GetModuleFileNameW(NULL,path,32768);if(!n||n>=32768)throw std::runtime_error("Invalid application path");directory.assign(path,n);directory.resize(directory.find_last_of(L"\\/")+1);
        wchar_t verify[32768];DWORD verifySize=GetEnvironmentVariableW(L"MOUNTAIN_EFFECT_VERIFY",verify,32768);if(verifySize&&verifySize<32768)logPath=verify;
        auto args=arguments();std::string json;bool provided=false,test=false,isolatedTest=false;
        for(size_t i=1;i<args.size();++i) {
            if(args[i]==L"--selftest")test=true;
            else if(args[i]==L"--accept-test-input")acceptTestInput=true;
            else if(args[i]==L"--test-isolated")isolatedTest=true;
            else if(args[i]==L"--verify"&&i+1<args.size())logPath=args[++i];
            else if(args[i]==L"--snapshot"&&i+1<args.size())snapshotPath=args[++i];
            else if(args[i]==L"--payload"&&i+1<args.size()) { json=base64(args[++i]);provided=true; }
        }
        if(test) { selfTest();return 0; }
        if(!provided)json=readFile(directory+L"settings.json");Settings settings=Settings::from(json);
        DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);windowClass=L"MountainNativeEffect-"+std::to_wstring(session);
        if(isolatedTest&&acceptTestInput)windowClass+=L"-test-"+std::to_wstring(GetCurrentProcessId());
        std::wstring mutexName=L"Local\\"+windowClass;mutex=CreateMutexW(NULL,FALSE,mutexName.c_str());if(!mutex)throw std::runtime_error("Cannot create effect mutex");
        for(int attempt=0;attempt<120;++attempt) {
            DWORD acquired=WaitForSingleObject(mutex,0);
            if(acquired==WAIT_OBJECT_0||acquired==WAIT_ABANDONED) { owner=true;break; }
            HWND existing=FindWindowW(windowClass.c_str(),NULL);
            if(existing) {
                COPYDATASTRUCT data={provided?1u:2u,DWORD(json.size()),const_cast<char*>(json.data())};DWORD_PTR response=0;
                if(SendMessageTimeoutW(existing,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&response)&&response==1) { log(provided?"PreviewForwarded":"AlreadyRunningNoAction");CloseHandle(mutex);return 0; }
            }
            Sleep(10);
        }
        if(!owner)throw std::runtime_error("Effect is not responding");
        HWND foreground=GetForegroundWindow();POINT testPoint;GetCursorPos(&testPoint);HWND underlying=WindowFromPoint(testPoint);
        Engine engine;activeEngine=&engine;
        engine.keyboardHook=SetWindowsHookExW(WH_KEYBOARD_LL,copilotHook,instance,0);
        if(!engine.keyboardHook)throw std::runtime_error("Cannot install Copilot key hook");
        engine.copilotHold.held=!provided&&(down(VK_LWIN)||down(VK_RWIN))&&(down(VK_LSHIFT)||down(VK_RSHIFT));
        engine.railInputHeld=engine.copilotHold.held;
        if(settings.effect=="railgun"){engine.rail.ready=true;engine.rail.holding=engine.copilotHold.held;engine.rail.settings=settings;}
        engine.startupEvents.reserve(32);
        engine.mouseHook=SetWindowsHookExW(WH_MOUSE_LL,railMouseHook,instance,0);
        if(!engine.mouseHook)throw std::runtime_error("Cannot install wheel capture hook");
        engine.holdStarted=now();
        log(std::string("CopilotHook=Ready InitialHeld=")+(engine.copilotHold.held?"true":"false"));
        engine.setupBuffer();WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=windowClass.c_str();RegisterClassExW(&wc);
        HWND window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,windowClass.c_str(),L"이게 코파일럿보다 낫다 · 효과",WS_POPUP,engine.left,engine.top,engine.width,engine.height,NULL,NULL,instance,&engine);
        if(!window)throw std::runtime_error("Cannot create effect window");
        log("Transparency=PerPixelAlpha");
        GetCursorPos(&engine.previousCursor);engine.leftDown=down(VK_LBUTTON);engine.middleDown=down(VK_MBUTTON);engine.spaceDown=down(VK_SPACE);engine.started=engine.previous=now();
        engine.trigger(settings,&bootCursor);
        for(const auto& event:engine.startupEvents){if(event.isWheel)engine.chargeRail(event.wheel);else engine.copilot(event.up,event.time,&event.position);}
        engine.startupEvents.clear();
        if(engine.settingsPending||(!provided&&down(VK_F12))){engine.settingsPending=false;engine.f12Captured=true;engine.requestSettings();}
        engine.paint(); // Present the first effect immediately, before entering the message loop.
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);UpdateWindow(window);SetTimer(window,1,16,NULL);
        log("NativeHost Style="+std::to_string(GetWindowLongPtrW(window,GWL_EXSTYLE)));
        log(std::string("FocusPreserved=")+(GetForegroundWindow()==foreground?"true":"false")+" HitTestPassThrough="+(WindowFromPoint(testPoint)==underlying?"true":"false"));
        MSG message;while(GetMessageW(&message,NULL,0,0)>0) { TranslateMessage(&message);DispatchMessageW(&message); }
        activeEngine=NULL;ReleaseMutex(mutex);CloseHandle(mutex);return 0;
    } catch(const std::exception& error) {
        if(owner)ReleaseMutex(mutex);if(mutex)CloseHandle(mutex);log(std::string("Error=")+error.what());
        if(logPath.empty())MessageBoxW(NULL,L"효과를 실행하지 못했습니다. settings.json과 실행 파일을 확인해 주세요.",L"이게 코파일럿보다 낫다",MB_OK|MB_ICONERROR);
        return 1;
    }
}


