import streamlit as st
import streamlit_antd_components as antd
import json


# Set page configuration
st.set_page_config(page_title="LLM Trace Detail Viewer", layout="wide")

# Title of the app
st.title("LLM Trace Detail Viewer")


# Main section
# https://nicedouble-streamlitantdcomponentsdemo-app-middmy.streamlit.app/

col1, col2 = st.columns([1,2])
with col1:
    antd.tree(items=[
        antd.TreeItem('item1', tag=[antd.Tag('Tag', color='red'), antd.Tag('Tag2', color='cyan')]),
        antd.TreeItem('item2', icon='apple', description='item description', children=[
            antd.TreeItem('tooltip', icon='github', tooltip='item tooltip'),
            antd.TreeItem('item2-2', children=[
                antd.TreeItem('item2-2-1'),
                antd.TreeItem('item2-2-2'),
                antd.TreeItem('item2-2-3'),
            ]),
        ]),
        antd.TreeItem('disabled', disabled=True),
        antd.TreeItem('item3', children=[
            antd.TreeItem('item3-1'),
            antd.TreeItem('item3-2'),
        ]),
    ], label='label', index=0, align='center', size='md', icon='table', open_all=True, checkbox=False)

    tracker_json = open("tracker.json").read()
    tracker = json.loads(tracker_json)

    def tracker_to_antd(routine):
        call_ts = routine['call_ts']
        ret_ts = routine.get('call_ts', routine.get('dump_ts'))
        used_ts = (ret_ts - call_ts) / 1000000
        return antd.TreeItem(
            label=routine.get('callee', routine.get('scope_name')), 
            tag=[antd.Tag(f"{used_ts}s"), antd.Tag("异步调用")],
            children=[tracker_to_antd(k) for k in routine.get('subroutines', [])]
        )

    antd_tree = tracker_to_antd(tracker)

    antd.tree(items=[antd_tree], label=tracker["scope_name"], index=0, align='center', size='md', open_all=True, checkbox=False)



with col2:
    st.write("### Trace Detail")
    st.write("**Session:** lf.docs.conversation.TL4KDIo  |  **User ID:** u-xNZclH1y   |    **Total cost:** $0.0002")

    st.write("HUMAN 09:00:01")
    st.code("你好")

    st.write("AI 09:01:01")
    st.code("你好")

    st.button("详情")
