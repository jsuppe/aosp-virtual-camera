package com.example.vcamproducer;

import android.opengl.GLES20;

/** Experimental render-once target. Single EGL owner/context; no external fences. */
final class SceneReuse implements AutoCloseable {
    private int texture, framebuffer, program;
    private final int vertices;
    SceneReuse(int width,int height,int vertices) {
        this.vertices=vertices;
        try {
            int[] ids=new int[1];
            GLES20.glGenTextures(1,ids,0);texture=ids[0];
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D,0,GLES20.GL_RGBA,width,height,0,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,null);
            GLES20.glGenFramebuffers(1,ids,0);framebuffer=ids[0];
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,framebuffer);
            GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER,GLES20.GL_COLOR_ATTACHMENT0,GLES20.GL_TEXTURE_2D,texture,0);
            if(GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER)!=GLES20.GL_FRAMEBUFFER_COMPLETE)
                throw new IllegalStateException("Incomplete reuse framebuffer");
            program=GLES20.glCreateProgram();
            int vs=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 aPos; varying vec2 uv; void main(){gl_Position=vec4(aPos,0.,1.);uv=aPos*.5+.5;}");
            int fs=shader(GLES20.GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 uv; uniform sampler2D scene; void main(){gl_FragColor=texture2D(scene,uv);}");
            GLES20.glAttachShader(program,vs);GLES20.glAttachShader(program,fs);
            GLES20.glBindAttribLocation(program,0,"aPos");GLES20.glLinkProgram(program);
            GLES20.glDeleteShader(vs);GLES20.glDeleteShader(fs);
            int[] ok=new int[1];GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,ok,0);
            if(ok[0]==0)throw new IllegalStateException(GLES20.glGetProgramInfoLog(program));
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,0);
        } catch(RuntimeException e){close();throw e;}
    }
    private static int shader(int type,String source){
        int id=GLES20.glCreateShader(type);GLES20.glShaderSource(id,source);GLES20.glCompileShader(id);
        int[] ok=new int[1];GLES20.glGetShaderiv(id,GLES20.GL_COMPILE_STATUS,ok,0);
        if(ok[0]==0){String error=GLES20.glGetShaderInfoLog(id);GLES20.glDeleteShader(id);throw new IllegalStateException(error);}return id;
    }
    void begin(){
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,framebuffer);
        GLES20.glClearColor(0,0,0,1);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
    }
    // Retains the simple-copy program/geometry for DirectMirror's subsequent draw.
    void toCamera(int width,int height){
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,0);
        GLES20.glViewport(0,0,width,height);
        GLES20.glClearColor(0,0,0,1);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(program);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE1);GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"scene"),1);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,vertices);
        GLES20.glEnableVertexAttribArray(0);GLES20.glVertexAttribPointer(0,2,GLES20.GL_FLOAT,false,0,0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
        int error=GLES20.glGetError();if(error!=GLES20.GL_NO_ERROR)throw new IllegalStateException("Reuse draw error="+error);
    }
    public void close(){
        if(framebuffer!=0)GLES20.glDeleteFramebuffers(1,new int[]{framebuffer},0);
        if(texture!=0)GLES20.glDeleteTextures(1,new int[]{texture},0);
        if(program!=0)GLES20.glDeleteProgram(program);
        framebuffer=texture=program=0;
    }
}
