# uncompyle6 version 3.9.3
# Python bytecode version base 3.7.0 (3394)
# Decompiled from: Python 3.9.6 (default, Jan  9 2026, 11:03:41) 
# [Clang 17.0.0 (clang-1700.6.4.2)]
# Embedded file name: Gui_DMC4.py
import tkinter as tk
from tkinter import ttk
import threading, DMC4_Weapon1 as D, psutil, pymem, pymem.memory
Mem = pymem.memory

class Menu:

    def __init__(self, Master):
        self.List = []
        self.Text_Game = tk.StringVar()
        self.Style = ttk.Style()
        self.Style.configure("MENU.TFrame", background="grey")
        self.Frame_Menu = ttk.Frame(Master, style="MENU.TFrame")
        self.Frame_Menu.pack(fill="x", ipady=5)
        self.Button_Debug = ttk.Button((self.Frame_Menu), text="Debug", command=(lambda: self.Debugger()))
        self.Button_Debug.pack(side="left", padx=5)

    def Debugger(self):
        try:
            Switcher.debug()
        except:
            pass


class DMC4_Switcher:

    def __init__(self, Master):
        self.Bool_Rebellion = tk.BooleanVar()
        self.Bool_Gilamesh = tk.BooleanVar()
        self.Bool_Lucifer = tk.BooleanVar()
        self.Bool_Ebony = tk.BooleanVar()
        self.Bool_Pandora = tk.BooleanVar()
        self.Bool_Pump = tk.BooleanVar()
        self.Style = ttk.Style()
        self.Style.configure("BG.TFrame", background="#383838")
        self.Frame = ttk.Frame(Master, style="BG.TFrame")
        self.Frame.pack(fill="both", expand=True)
        self.Frame_Title = ttk.Frame((self.Frame), style="BG.TFrame")
        self.Frame_Title.pack(fill="x", ipady=3)
        self.Display_Title = tk.Label((self.Frame_Title), bg="#383838", text="Melee Weapon ", fg="white", font=('Comic Sans MS',
                                                                                                                10))
        self.Display_Title.pack()
        self.Check_Rebellion = tk.Checkbutton((self.Frame), text="Disable Rebellion", bg="#383838", variable=(self.Bool_Rebellion), activebackground="#383838",
          command=(lambda: self.Disable_Rebellion(Master)))
        self.Check_Rebellion.pack(anchor="w")
        self.Check_Gilgamesh = tk.Checkbutton((self.Frame), text="Disable Gilgamesh", bg="#383838", variable=(self.Bool_Gilamesh), activebackground="#383838",
          command=(lambda: self.Disable_Gilgamesh(Master)))
        self.Check_Gilgamesh.pack(anchor="w")
        self.Check_Lucifer = tk.Checkbutton((self.Frame), text="Disable Lucifer", bg="#383838", variable=(self.Bool_Lucifer), activebackground="#383838",
          command=(lambda: self.Disable_Lucifer(Master)))
        self.Check_Lucifer.pack(anchor="w")
        self.Frame_Weapon = ttk.Frame((self.Frame), style="BG.TFrame")
        self.Frame_Weapon.pack(fill="x", ipady=5)
        self.Display_Title_Weapon = tk.Label((self.Frame_Weapon), text="ranged weapons", bg="#383838", fg="white", font=('Comic Sans MS',
                                                                                                                         10))
        self.Display_Title_Weapon.pack()
        self.Check_Pump = tk.Checkbutton((self.Frame), text="Disable Shotgun", bg="#383838", variable=(self.Bool_Pump),
          activebackground="#383838",
          command=(lambda: self.Disable_Pump(Master)))
        self.Check_Pump.pack(anchor="w")
        self.Check_Pandora = tk.Checkbutton((self.Frame), text="Disable Pandora", bg="#383838", variable=(self.Bool_Pandora),
          activebackground="#383838",
          command=(lambda: self.Disable_Pandora(Master)))
        self.Check_Pandora.pack(anchor="w")
        self.Check_Ebony = tk.Checkbutton((self.Frame), text="Disable Ebony", bg="#383838", variable=(self.Bool_Ebony),
          activebackground="#383838",
          command=(lambda: self.Disable_Ebony(Master)))
        self.Check_Ebony.pack(anchor="w")
        self.Frame_Error = tk.Frame((self.Frame), bg="grey")
        self.Frame_Error.pack(side="bottom", fill="x", ipady=5)
        self.Display_Error = tk.Label((self.Frame_Error), bg="grey", text="this mod does not work\n if you use another trainer running a DLL Injection\n the game may crash.\n Only works with the special edition",
          fg="#700700",
          font=('Comic Sans MS', 10))
        self.Display_Error.pack()

    def Disable_Rebellion(self, Master):
        if self.Bool_Rebellion.get() == True:
            Master.after(1, lambda: self.Disable_Rebellion(Master))
            try:
                self.Bool_Lucifer.set(False)
                self.Bool_Gilamesh.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_right_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Right_Weapon)
                    if Equipped_Weapon == 4:
                        Mem.write_int(Switcher.Handle, Switcher.Right_Weapon, 5)
            except:
                pass

    def Disable_Gilgamesh(self, Master):
        if self.Bool_Gilamesh.get() == True:
            Master.after(1, lambda: self.Disable_Gilgamesh(Master))
            try:
                self.Bool_Rebellion.set(False)
                self.Bool_Lucifer.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_right_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Right_Weapon)
                    if Equipped_Weapon == 5:
                        Mem.write_int(Switcher.Handle, Switcher.Right_Weapon, 6)
            except:
                pass

    def Disable_Lucifer(self, Master):
        if self.Bool_Lucifer.get() == True:
            Master.after(1, lambda: self.Disable_Lucifer(Master))
            try:
                self.Bool_Gilamesh.set(False)
                self.Bool_Rebellion.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_right_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Right_Weapon)
                    if Equipped_Weapon == 6:
                        Mem.write_int(Switcher.Handle, Switcher.Right_Weapon, 4)
            except:
                pass

    def Disable_Pump(self, Master):
        if self.Bool_Pump.get() == True:
            Master.after(1, lambda: self.Disable_Pump(Master))
            try:
                self.Bool_Ebony.set(False)
                self.Bool_Pandora.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_left_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Left_Weapon)
                    if Equipped_Weapon == 7:
                        Mem.write_int(Switcher.Handle, Switcher.Left_Weapon, 8)
            except:
                pass

    def Disable_Pandora(self, Master):
        if self.Bool_Pandora.get() == True:
            Master.after(1, lambda: self.Disable_Pandora(Master))
            try:
                self.Bool_Pump.set(False)
                self.Bool_Ebony.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_left_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Left_Weapon)
                    if Equipped_Weapon == 8:
                        Mem.write_int(Switcher.Handle, Switcher.Left_Weapon, 9)
            except:
                pass

    def Disable_Ebony(self, Master):
        if self.Bool_Ebony.get() == True:
            Master.after(1, lambda: self.Disable_Ebony(Master))
            try:
                self.Bool_Pandora.set(False)
                self.Bool_Pump.set(False)
                Switcher.get_entity()
                Entity = Mem.read_int(Switcher.Handle, Switcher.Entity)
                if Entity == 16943776:
                    Switcher.get_left_weapon()
                    Equipped_Weapon = Mem.read_int(Switcher.Handle, Switcher.Left_Weapon)
                    if Equipped_Weapon == 9:
                        Mem.write_int(Switcher.Handle, Switcher.Left_Weapon, 7)
            except:
                pass


WIN = tk.Tk()
WIN.resizable(0, 0)
WIN.title("DMC4-SE Dante Weapon Disabler MOD By Dega V0.4")
WIN.geometry("450x360")
WIN.iconbitmap("ICON.ico")
MENU = Menu(WIN)
APP = DMC4_Switcher(WIN)
Switcher = D.DMC4()
Switcher.debug()
WIN.mainloop()

# okay decompiling Gui_DMC4.pyc
