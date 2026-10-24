package com.example.vcamproducer;
import java.io.*;
import java.nio.file.*;
import java.util.List;
public final class FrameTraceTest {
    public static void main(String[] args) throws Exception {
        Path dir=Files.createTempDirectory("vcam-frame-trace-");
        FrameTrace off=new FrameTrace(dir.toFile(),"off","a,b,c,d,e,f,g,h",false);
        off.add(1,2,3,4,5,6,7,8);off.close();
        if(Files.exists(dir.resolve("off.csv")))throw new AssertionError("disabled trace wrote a file");
        FrameTrace on=new FrameTrace(dir.toFile(),"on","a,b,c,d,e,f,g,h",true);
        for(int i=0;i<16385;i++)on.add(i,2,3,4,5,6,7,8);
        on.close();on.close();
        List<String> lines=Files.readAllLines(dir.resolve("on.csv"));
        if(lines.size()!=16386 || !lines.get(0).endsWith("dropped=1") ||
                !lines.get(2).equals("0,2,3,4,5,6,7,8") ||
                !lines.get(lines.size()-1).equals("16383,2,3,4,5,6,7,8"))
            throw new AssertionError("trace bounds/encoding");
        Files.delete(dir.resolve("on.csv"));Files.delete(dir);
        System.out.println("PASS: app trace opt-in, bounded samples, exact CSV and idempotent close");
    }
}
