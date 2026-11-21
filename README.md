Build (x86 developer prompt):  cl /LD /O2 /MT ItemDropD2.cpp user32.lib
Load:  PlugY.ini -> DllToLoad=ItemDropD2.dll

Offline / single player only. Do not load on Battle.net. Keep a backup of your characters.

How it works:

A hook on the client's chat input (D2Client+0x70B75, chained to BH.dll) catches ".item ...".
    
The request is queued.
    
A detour at the start of D2Game's packet dispatcher (D2Game+0xCC550) runs on the server thread. 

There the queue is drained and D2Game's own ground-drop wrapper (D2Game+0x11880) creates the item at the player.
    
Every address is verified against the expected bytes before it is patched or called.

How to use:

Put the compiled .dll next to your Plugy installation.

Open Plugy.ini and look for DllToLoad=

Add the name of your compiled .dll to that line (for example DllToLoad=ItemDropD2.dll)

Run Diablo II and type in chat .item uap unique this should spawn a unique shako

Works for each quality. normal, magic, rare, unique and set
