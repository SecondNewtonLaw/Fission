//
// Created by Dottik on 2/6/2026.
//

// Complex control-flow stress tests (deep if/elseif chains, nested loops, break/continue).

#include "IntegrationTestSupport.hpp"

static const std::string kComplexEnvironment =
    R"ENV(for _,name in {"fa","fb","fc","fd","use","pickCustom","pickTrack","legacy","warn","doMode","doOcclusion","doZoom","lock","classic","zoom","leaf1","leaf2","leaf3","leaf4","leaf5","leaf6","p","q","r","s","inner","outer","done","step","body","tail","mark","work","more","after","a","b","c","d","tick","visit","finish"} do _G[name]=function(...) print(name,...); return name end end
cond=function(i,j) print("cond",i,j); return j==1 end
stop=function() print("stop"); return stopEarly end)ENV";

TEST_CASE("CCF: terminating branch join does not duplicate property store", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local t = { value = false }
        local function f(flag)
            local value
            if flag then
                value = true
            else
                value = false
            end
            t.value = value
            return t
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(true).value,f(false).value,f(nil).value))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: four-way elseif chain preserves every branch once", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(x)
            if x == 1 then
                fa()
            elseif x == 2 then
                fb()
            elseif x == 3 then
                fc()
            else
                fd()
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for x=0,4 do f(x) end)DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: a-and-b-or-c preserves short-circuit values", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(a, b, c)
            local x = a and b or c
            print(x)
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,a in {false,true} do for _,b in {false,true} do f(a,b,"fallback") end end; f(nil,7,9); f(0,nil,11))DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: and-call-or preserves namecall effects", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(self, cs)
            self.x = cs and cs:IsA("VehicleSeat") or false
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); local self={}; f(self,nil); print(self.x); for _,answer in {false,true} do f(self,{IsA=function(_,name) print(name); return answer end}); print(self.x) end)DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: assignment elseif chain preserves branch values", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(x, flag)
            local mode = 0
            if x == 1 then
                mode = 10
            elseif x == 2 then
                mode = 20
            elseif x == 3 then
                mode = 30
            elseif x == 4 then
                mode = 40
            else
                mode = 99
            end
            if flag then
                use(mode)
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for x=0,5 do f(x,true); f(x,false) end)DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: or-grouped elseif does not leak into the else branch", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t)
            local creator = nil
            if t == "Scriptable" then
                return
            elseif t == "Custom" then
                creator = pickCustom()
            elseif t == "Track" then
                creator = pickTrack()
            elseif t == "Attach" or t == "Watch" or t == "Fixed" then
                creator = legacy()
            else
                warn("unhandled", t)
            end
            use(creator)
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,kind in {"Scriptable","Custom","Track","Attach","Watch","Fixed","Unknown"} do f(kind) end)DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: elseif chain with no-op tail branches", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(name)
            if name == "Mode" then
                doMode()
            elseif name == "Occlusion" then
                doOcclusion()
            elseif name == "Zoom" then
                doZoom()
            elseif name == "A" then
            elseif name == "B" then
            elseif name == "C" then
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,kind in {"Mode","Occlusion","Zoom","A","B","C","Unknown"} do f(kind) end)DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: nested if inside an elseif arm", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(prop, mode)
            if prop == "CameraMode" then
                if mode == "LockFirstPerson" then
                    lock()
                elseif mode == "Classic" then
                    classic()
                else
                    warn("bad", mode)
                end
            elseif prop == "Zoom" then
                zoom()
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,prop in {"CameraMode","Zoom","Other"} do for _,mode in {"LockFirstPerson","Classic","Other"} do f(prop,mode) end end)DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: three-deep nested if/else keeps all leaves", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(a, b, c)
            if a then
                if b then
                    if c then leaf1() else leaf2() end
                else
                    if c then leaf3() else leaf4() end
                end
            else
                if b then leaf5() else leaf6() end
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,a in {false,true} do for _,b in {false,true} do for _,c in {false,true} do f(a,b,c) end end end)DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: elseif chain returning per-branch values", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(x)
            local r
            if x == 1 then
                r = "one"
            elseif x == 2 then
                r = "two"
            elseif x == 3 then
                r = "three"
            else
                r = "other"
            end
            return r
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for x=0,4 do print(f(x)) end)DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: compound boolean conditions in elseif arms", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(a, b, c)
            if a and b then
                p()
            elseif a or c then
                q()
            elseif not a and not b and not c then
                r()
            else
                s()
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,a in {false,true} do for _,b in {false,true} do for _,c in {false,true} do f(a,b,c) end end end)DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: nested while with inner break", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(n, m)
            local i = 0
            while i < n do
                local j = 0
                while j < m do
                    if cond(i, j) then
                        break
                    end
                    inner(i, j)
                    j = j + 1
                end
                outer(i)
                i = i + 1
            end
            done()
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); f(0,3); f(2,0); f(2,3))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: repeat-until with conditional break", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local x = 0
            repeat
                x = x + 1
                if x == 5 then
                    break
                end
                step(x)
            until x >= 10
            return x
        end
        return f()
    )LUA",
        R"DRIVER(print(__integration_subject()))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: while with a nested repeat-until", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(n)
            local i = 0
            while i < n do
                local k = 0
                repeat
                    k = k + 1
                    body(i, k)
                until k >= 3
                tail(i)
                i = i + 1
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); f(0); f(2))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: numeric-for with nested while and early return", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t)
            for i = 1, #t do
                local v = t[i]
                while v > 0 do
                    if v == 13 then
                        return i
                    end
                    v = v - 1
                end
                mark(i)
            end
            return -1
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f({}),f({0,2}),f({14,1}),f({1,13})))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: while-true with multiple break conditions", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(a, b)
            while true do
                work()
                if a() then
                    break
                end
                if b() then
                    break
                end
                more()
            end
            after()
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); local count=0; f(function() count+=1; print("a",count); return count==2 end,function() print("b",count); return false end); count=0; f(function() count+=1; print("a",count); return false end,function() print("b",count); return count==2 end))DRIVER",
        kComplexEnvironment
    );
}

TEST_CASE("CCF: for over while over elseif with break", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(rows)
            for i = 1, rows do
                local j = 0
                while j < 10 do
                    if j == 3 then
                        a()
                    elseif j == 7 then
                        b()
                        break
                    else
                        c()
                    end
                    j = j + 1
                end
                d(i)
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); f(0); f(2))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: repeat-until with compound or condition", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local x = 0
            repeat
                x = x + 1
                tick(x)
            until x >= 10 or stop()
            return x
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); stopEarly=false; print(f()); stopEarly=true; print(f()))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("CCF: generic-for with conditional skip", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t)
            for k, v in pairs(t) do
                if v == nil then
                    continue
                end
                if v == "stop" then
                    break
                end
                visit(k, v)
            end
            finish()
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); f({}); f({"a","b"}); f({"first","stop","last"}))DRIVER", kComplexEnvironment
    );
}

TEST_CASE("Regress: or/and short-circuit chain feeding a table index preserves execution at O0", "[Decompiler][ControlFlow][Integration]") {
    integration_test::Check(
        R"LUA(local t = {["hello"] = 1}
print(t[("h" .. "e" .. "ll" .. "o") or #t or #("halo" .. "halo") and _])
)LUA",
        R"DRIVER()DRIVER", kComplexEnvironment
    );
}
