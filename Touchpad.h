#pragma once
// The supplied LG/04CA00A0 prototype uses five slots in a 30-byte report.
struct PadContact { int slot; double x,y; };
static bool parsePad(const BYTE* data,size_t size,std::vector<PadContact>& out) {
    out.clear();if(size!=30||data[0]!=1)return false;
    for(int slot=0;slot<5;++slot){size_t at=1+slot*5;unsigned flags=data[at];
        unsigned x=data[at+1]|(unsigned(data[at+2])<<8),y=data[at+3]|(unsigned(data[at+4])<<8);
        if(flags&2){if(x>0x05FB||y>0x03BC)continue;out.push_back({int(flags>>2),double(x)/0x05FB,double(y)/0x03BC});}
    }return true;
}
