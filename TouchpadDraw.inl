    bool drawing=false,drawDirty=false,penDown=false;
    std::map<int,POINT> inkHeads;
    int penSlot=-1;double drawOpened=0;POINT inkLast={};
    HDC inkDC=NULL;HBITMAP inkBitmap=NULL;HGDIOBJ inkOld=NULL;RECT board={};
    void freeInk(){if(inkDC){SelectObject(inkDC,inkOld);DeleteDC(inkDC);inkDC=NULL;}if(inkBitmap){DeleteObject(inkBitmap);inkBitmap=NULL;}}
    void openBoard(const Bounds& bounds){
        freeInk();int mw=int(bounds.right-bounds.left),mh=int(bounds.bottom-bounds.top);
        bool screen=padSettings.drawingSurface=="screen";int bw=screen?mw:int(mw*.8),bh=screen?mh:int(mh*.8);board={LONG(bounds.left+(mw-bw)/2),LONG(bounds.top+(mh-bh)/2),LONG(bounds.left+(mw+bw)/2),LONG(bounds.top+(mh+bh)/2)};
        inkDC=CreateCompatibleDC(buffer);inkBitmap=CreateCompatibleBitmap(buffer,bw,bh);
        if(!inkDC||!inkBitmap){freeInk();throw std::runtime_error("Cannot create drawing canvas");}
        inkOld=SelectObject(inkDC,inkBitmap);RECT rect={0,0,bw,bh};HBRUSH background=CreateSolidBrush(screen?RGB(0,0,0):RGB(245,246,250));FillRect(inkDC,&rect,background);DeleteObject(background);
        if(!screen){RECT title={0,0,bw,48};HBRUSH bar=CreateSolidBrush(RGB(28,33,46));FillRect(inkDC,&title,bar);DeleteObject(bar);
        HGDIOBJ oldFont=SelectObject(inkDC,font);SetBkMode(inkDC,TRANSPARENT);SetTextColor(inkDC,RGB(232,237,248));
        const wchar_t* hint=L"터치패드 그림판 · 한 손가락으로 그리기 · Copilot 다시 누르기 / Esc로 닫기";
        TextOutW(inkDC,16,15,hint,lstrlenW(hint));SelectObject(inkDC,oldFont);}
        drawing=true;drawDirty=true;penDown=false;penSlot=-1;inkHeads.clear();drawOpened=now();log("Drawing=Opened");
    }
    void drawContacts(const std::vector<PadContact>& contacts){
        if(!drawing)return;
        int bw=board.right-board.left,bh=board.bottom-board.top;bool screen=padSettings.drawingSurface=="screen";int marginTop=screen?12:60;
        std::map<int,POINT> nextHeads;
        HPEN pen=CreatePen(PS_SOLID,3,screen?RGB(255,105,180):RGB(38,65,118));HGDIOBJ old=SelectObject(inkDC,pen);
        for(const PadContact& c:contacts){POINT point={LONG(12+c.x*(bw-24)),LONG(marginTop+c.y*(bh-marginTop-12))};
            auto head=inkHeads.find(c.slot);
            if(head!=inkHeads.end()){MoveToEx(inkDC,head->second.x,head->second.y,NULL);LineTo(inkDC,point.x,point.y);}
            else{MoveToEx(inkDC,point.x,point.y,NULL);LineTo(inkDC,point.x+1,point.y);}
            nextHeads[c.slot]=point;inkLast=point;penSlot=c.slot;
        }
        SelectObject(inkDC,old);DeleteObject(pen);inkHeads.swap(nextHeads);penDown=!contacts.empty();if(penDown)drawDirty=true;

    }
