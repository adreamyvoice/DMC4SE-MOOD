# uncompyle6 version 3.9.3
# Python bytecode version base 3.7.0 (3394)
# Decompiled from: Python 3.9.6 (default, Jan  9 2026, 11:03:41) 
# [Clang 17.0.0 (clang-1700.6.4.2)]
# Embedded file name: DMC4_Weapon1.py
import pymem, pymem.memory, time
Mem = pymem.memory

class DMC4:

    def __init__(self):
        self.Left_Weapon = None
        self.Right_Weapon = None
        self.Entity = None
        self.DLL = None
        self.Handle = None

    def get_left_weapon(self):
        try:
            self.Left_Weapon = self.resolve_pointer(self.Handle, self.DLL, 15567580, [356], 9116)
            return self.Left_Weapon
        except:
            pass

    def get_right_weapon(self):
        try:
            self.Right_Weapon = self.resolve_pointer(self.Handle, self.DLL, 15567580, [360], 9112)
            return self.Right_Weapon
        except:
            pass

    def get_entity(self):
        try:
            self.Entity = self.resolve_pointer(self.Handle, self.DLL, 15567580, [360], 0)
            return self.Entity
        except:
            pass

    def resolve_pointer(self, handle, dll, base, list, final):
        address = Mem.read_int(handle, dll + base)
        for offset in list:
            address = Mem.read_uint(handle, address + offset)
            address = address + final

        return address

    def debug(self):
        try:
            PM = pymem.Pymem("DevilMayCry4SpecialEdition")
            self.DLL = PM.process_base.lpBaseOfDll
            self.Handle = PM.process_handle
        except:
            pass

# okay decompiling DMC4_Weapon1.pyc
