    bool padRail=false;std::map<int,PadContact> railPadHeads;double railPadCredit=0;
    void railPadInput(const std::vector<PadContact>& contacts){
        if(!padRail||!rail.ready||!rail.holding||cancelUntilUp||contacts.size()!=2){railPadHeads.clear();railPadCredit=0;return;}
        std::map<int,PadContact> next;for(const auto& c:contacts)next[c.slot]=c;
        if(next.size()!=2){railPadHeads.clear();return;}
        if(railPadHeads.size()==2&&railPadHeads.count(next.begin()->first)&&railPadHeads.count(next.rbegin()->first)){
            double motion=0;for(const auto& item:next){const auto& prev=railPadHeads[item.first];motion+=std::hypot(item.second.x-prev.x,(item.second.y-prev.y)*.625)/2;}
            // Ignore tiny stationary jitter; keep the reference until meaningful motion.
            if(motion<.0012)return;
            if(motion<.15){railPadCredit+=motion/.035*120;int delta=int(railPadCredit);railPadCredit-=delta;
                if(delta){chargeRailAmount(delta);if(padPackets%60==0)log("Rail=TouchpadCharge Energy="+std::to_string(rail.energy));}}
        }
        railPadHeads.swap(next);
    }
    bool padReady=false;double padUntil=0,padLastReport=-100,padEmission=0;
    Settings padSettings;Bounds padBounds={};std::vector<PadContact> padContacts;unsigned padPackets=0;
    void padInput(LPARAM handle) {
        if(!padReady)return;
        UINT size=0;if(GetRawInputData((HRAWINPUT)handle,RID_INPUT,NULL,&size,sizeof(RAWINPUTHEADER))==UINT(-1)||size<sizeof(RAWINPUTHEADER)||size>65536)return;
        std::vector<BYTE> bytes(size);if(GetRawInputData((HRAWINPUT)handle,RID_INPUT,bytes.data(),&size,sizeof(RAWINPUTHEADER))!=size)return;
        const RAWINPUT* raw=(const RAWINPUT*)bytes.data();if(raw->header.dwType!=RIM_TYPEHID)return;
        RID_DEVICE_INFO info={};info.cbSize=sizeof(info);UINT infoSize=sizeof(info);
        if(GetRawInputDeviceInfoW(raw->header.hDevice,RIDI_DEVICEINFO,&info,&infoSize)==UINT(-1)||info.dwType!=RIM_TYPEHID||info.hid.usUsagePage!=0x0D||info.hid.usUsage!=5)return;
        size_t offset=offsetof(RAWINPUT,data.hid.bRawData);DWORD length=raw->data.hid.dwSizeHid,count=raw->data.hid.dwCount;
        if(length!=30||offset>size||count>(size-offset)/length)return;
        for(DWORD i=0;i<count;++i){std::vector<PadContact> contacts;if(!parsePad(raw->data.hid.bRawData+i*length,length,contacts))continue;
            railPadInput(contacts);drawContacts(contacts);padContacts=contacts;padLastReport=now();if(!contacts.empty())padUntil=now()+8;
            if(++padPackets==1||padPackets%120==0)log("TouchpadPackets="+std::to_string(padPackets)+" Contacts="+std::to_string(contacts.size()));
        }
    }
    void padStep(double time,double dt) {
        if(!padReady)return;
        if(time-padLastReport>.25){padContacts.clear();penDown=false;penSlot=-1;inkHeads.clear();railPadHeads.clear();railPadCredit=0;}
        if(padRail){if(!rail.ready){padReady=false;padContacts.clear();railPadHeads.clear();}return;}
        if(drawing)return;
        if(time>=padUntil&&padContacts.empty()){padReady=false;log("Touchpad=Idle");return;}
        padEmission+=dt*75;int count=int(padEmission);padEmission-=count;
        for(const PadContact& c:padContacts){double x=padBounds.left+24+c.x*(padBounds.right-padBounds.left-48),y=padBounds.top+24+c.y*(padBounds.bottom-padBounds.top-48);
            COLORREF color=colorForFixed(padSettings.palette);
            for(int i=0;i<count;++i){Particle p;p.kind=2;p.x=x;p.y=y;double angle=random01()*6.28318530718,speed=25+random01()*95;p.vx=std::cos(angle)*speed;p.vy=std::sin(angle)*speed;p.life=.5+random01()*.7;p.born=time;p.size=1+random01()*2.5;p.fall=0;p.drag=1;p.color=color;particles.push_back(p);}
        }
    }
