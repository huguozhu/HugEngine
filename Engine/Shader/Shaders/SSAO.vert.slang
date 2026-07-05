struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };
VSOutput vertexMain(uint vid:SV_VertexID){
    float2 pos[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    VSOutput o;o.p=float4(pos[vid],0,1);o.uv=float2(pos[vid].x*0.5+0.5,0.5-pos[vid].y*0.5);return o;
}
