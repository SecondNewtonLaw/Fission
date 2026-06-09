# Fission

### Background Knowledge

Fission is a Luau decompiler built from scratch for my closed source project, **RbxCli**. This project originally began as a gag to try and build a decompiler. It originally was meant to be the worst quickest fix possible in an argument of who can make the best decompiler in a short time frame.

However, given that there were already decompilers such as Medal, I thought that it would not really be that meaningful to make my own decompiler, but I was promptly proving wrong, as Konstant has not received any updates in quite the time, Medal is clearly not being developed and the only decent decompiler that remained in the landscape was nothing but Oracle, paid.

So, after some months of work, here we are.

### State of Development

As of right now, Fission is being actively developed. When I find a sample that breaks I try to fix it, somewhat. Currently, it supports `integer` and other Luau specifics. I have taken the time to ensure the quality of it post rework, however no real thing can be guaranteed when dealing with stuff like this.

Fission can (as of right now) decompile *most* loop and conditional structures without falling apart, and also has type inferrance and name inferrance, they're not perfect, however they make the trick!

### Disclaimer

Fission takes no code from other decompilers, however may reference Luau source code where applicable and/or ask the AI overlords for help.

### Credits

While Fission is provided without much churn, we at least request basic credits when using the software, pointing **specifically** to this repository.


#### Usage of AI

Fission has used AI in its development to improve and increase the quality of tests and other components. We have taken sufficient time to ensure the code generated has met the standards of quality we were after.