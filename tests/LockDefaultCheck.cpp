#include "Sequencer.h"
#include "FactoryContent.h"
#include <cstdio>
int main(){
    auto names = Factory::mixNames(); auto cats = Factory::mixCategories();
    int locked=0, unlocked=0, wrong=0;
    for (int i=0;i<names.size();++i){
        auto* ch=new DrumChannel(); Factory::applyMix(*ch,i);
        bool anyPitched=false, anyLocked=false;
        for (auto& sl: ch->slots) if (sl.engine==DrumChannel::SrcOsc||sl.engine==DrumChannel::SrcPhys||sl.engine==DrumChannel::SrcModal){ anyPitched=true; if(sl.lockPitch) anyLocked=true; }
        const bool melodic = cats[i]=="Bass"||cats[i]=="Keys"||cats[i]=="Pads & Choirs"||cats[i]=="Leads"||cats[i]=="Plucks & Strings"||cats[i]=="Bells & Mallets"||cats[i]=="Chords & Arps";
        if(anyPitched){ if(anyLocked)++locked; else ++unlocked;
            if(melodic==anyLocked){++wrong; printf("WRONG: %s (%s) locked=%d\n",names[i].toRawUTF8(),cats[i].toRawUTF8(),(int)anyLocked);} }
        delete ch;
    }
    printf("locked=%d unlocked=%d wrong=%d\n",locked,unlocked,wrong);
    return 0;
}
